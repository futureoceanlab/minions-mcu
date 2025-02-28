/* --------------------------------------------------------------------------
*   Minions-cam: program to run stereo pair on Minions floats
 *   May 12, 2020
 *   Authors: Junsu Jang, FOL/MIT
 *      Description: 
 *   
 *   Minions-cam is intended for Linux based embedded SBC to 
 *   control the stereo camera on Minions floats. 
 *   This firmware has three jobs: 
 *   
 *   1. Trigger images and strobe LEDs accordingly at specified 
 *      framerate
 * 
 *   2. Log relevant sensor data
 * 
 *   3. Save images accordingly once the camera has reached 
 *      below 20m 
 * 
 *   4. Synchronize time with the slave camera
 * 
 *   Jobs 1 and 2 are done by a timer interrupt, which toggles
 *   a flag. Inside the main loop, appropriate GPIO pins are
 *   toggled and sensor data is logged on a CSV file.
 * 
 *   Jobs 2 happens when the images arrive through the USB, images
 *   are saved along with the timestamp.
 * 
 *   Job 4 is processed in the main loop and requires another
 *   timer interrupt with 6 hour long interval. B connects
 *   to the WiFi hosted by A, and A runs a script that ssh
 *   into B and sets the clock on bash
 *   
*/


/* --
 * Mission details require following information
 *   - Deployment start depth (bar)
 *   - Framerate (fps)
 *   - Time synchronization interval (sec)
 *   - sensor measurement rate (regular) (sec (period))
 *   - Post deployment sensor measurement rate (sec (period))
 */

#include <iostream>
#include <stdexcept>
#include <string>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include "synchronization.h"
#include "peripheral.h"
#include "logger.h"



#define PERIOD 1 
#define MIN 60
#define TEN_MIN 600
#define OFFSET 2 

Peripheral *peripheral = new Peripheral(1);
Logger *logger = new Logger();

int count = 0;
uint8_t fTrig = 0, fSync = 0, fDrift = 0;
std::string t_rtc;
timer_t cameraTimerID, syncTimerID, driftTimerID;
struct timespec now;
long long T_skew_prev, T_skew_now;

void triggerCamera();

static void timer_handler(int sig, siginfo_t *si, void *uc)
{
    timer_t *tidp;
    tidp = (timer_t *) si->si_value.sival_ptr;
    if ( *tidp == cameraTimerID )
    {
		//auto start = std::chrono::steady_clock::now();
        triggerCamera();
        //fTrig = 1;
		//auto finish = std::chrono::steady_clock::now();
		//std::cout << "Handler: ";
		//std::cout << std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count() << std::endl;
    }
    else if ( *tidp == syncTimerID )
    {
        fSync = 1;
    }
    else if ( *tidp == driftTimerID )
    {
        fDrift = 1;
    }
}


void triggerCamera()
{
    // trigger camera
    peripheral->triggerOn();
    // Save timestamp, depth and temperature, frame id
    clock_gettime(CLOCK_MONOTONIC, &now);
	long long now_n = as_nsec(&now);
	//now_n += T_skew_now;
	t_rtc = "abc";
	t_rtc.pop_back();
	logger->log(now_n, t_rtc, 0.f, 0.f); //peripheral->getPressure(),  peripheral->getTemperature());
	peripheral->triggerOff();
	count++;
	//std::cout<<"TICK" << std::endl;
}


void setup()
{
    if (peripheral->init() == -1)
    {
        printf("error connecting to peripherals\n");
        return;
    }
    // CSV setup
    std::string logName="changeme.csv";
    logger->open(logName);

}


int makeTimer(std::string name, timer_t *timerID, struct timespec *T_start, int it_sec, int it_nsec)
{
    // varaibles
    struct sigaction act;
    struct sigevent te;
    struct itimerspec tim_spec = {.it_interval= {.tv_sec=it_sec,.tv_nsec=it_nsec},
                    .it_value = *T_start};
	std::cout << "ST: " << T_start->tv_sec << ", " << T_start->tv_nsec << std::endl;
	std::cout << "IT: " << it_sec << ", " << it_nsec << std::endl;
    // Signals 
    act.sa_flags = SA_SIGINFO | SA_RESTART;
    act.sa_sigaction = &timer_handler;
    sigemptyset(&act.sa_mask);

    if (sigaction(SIGALRM, &act, NULL) == -1)
    {
        fprintf(stderr, "Minions: Failed to setup signal handling for %s.\n", name);
        return -1;
    }

    // Timer setup
    te.sigev_notify = SIGEV_SIGNAL;
    te.sigev_signo = SIGALRM;
    te.sigev_value.sival_ptr = timerID;
    if (timer_create(CLOCK_MONOTONIC, &te, timerID))
    {
        perror("timer_create");
        return -1;
    }
    
    if (timer_settime(*timerID, TIMER_ABSTIME, &tim_spec, NULL))
    {
        perror("timer_settime"); 
        return -1;
    }
    return 0;

}

void resetTimer(timer_t *timerID, struct timespec *T_start, long long t_nsec)
{
    int it_sec = (int) (t_nsec / BILLION);
    int it_nsec = (int) (t_nsec % BILLION);
    struct itimerspec tim_spec = {.it_interval= {.tv_sec=it_sec,.tv_nsec=it_nsec},
                    .it_value = *T_start};
    if (timer_settime(*timerID, TIMER_ABSTIME, &tim_spec, NULL))
        perror("timer_settime"); 
}

int main(int argc, char* argv[])
{
    long long T_trig_n, T_sync_n, T_drift_n;
    int trig_period = PERIOD, drift_period = 61, sync_period = 301;
    int status;
    struct timespec T_trig, T_sync, T_drift;
    long long server_sec = BILLION;
    // TODO: What to do if power goes off intermittently and reboots in the 
    // mean time? Wait until connect to server and re-initiate
    setup();
    // 1. synchronize the time to that of the server, and make sure
    // we start triggering at the same time.
    struct timeinfo TI = {.T_skew_n = 0, .T_start_n = 0};
    if (synchronize(&TI, 1) == -1) 
    {
        printf("Sychronization error\n");
        exit(1);
    }
    // 2. Setup trigger, drift and sychronization timer
    //  Do sync and drift timer 250ms after every second so that no
    //  conflict happens

    // Trigger
    as_timespec(TI.T_start_n, &T_trig);
	std::cout << T_trig.tv_nsec << std::endl;
    //int status = clock_gettime(CLOCK_REALTIME, &T_trig);
    status = makeTimer("Trigger Timer", &cameraTimerID, &T_trig, PERIOD, 0);
    printf("status: %d\n", status);

    T_trig_n = TI.T_start_n;
    T_skew_now = TI.T_skew_n;
	//std::cout << T_skew_now << std::endl;


    // Drift
    T_drift_n = T_trig_n + drift_period*server_sec + server_sec/OFFSET;
    as_timespec(T_drift_n, &T_drift);
    status = makeTimer("Drift Timer", &driftTimerID, &T_drift, 0, 0); //MIN, server_sec/4);
    printf("status: %d\n", status);

    // Synchronization
    T_sync_n = T_trig_n + sync_period*server_sec + server_sec/OFFSET;
    std::cout<< T_sync_n << std::endl;
    as_timespec(T_sync_n, &T_sync);
    status = makeTimer("Sync Timer", &syncTimerID, &T_sync, 0, 0); //TEN_MIN, server_sec/4);
    T_sync_n = T_trig_n;
    printf("status: %d\n", status);
    printf("Start loopin\n"); 


    // Routine for timer handling
    while (1)
    {
        // Log upon triggering
/*        if (fTrig)
        {
            //peripheral->readData();
            fTrig = 0;
            //std::cout << "tick" << std::endl;
        }*/

        // set time for 10 min synchronization
        if (fSync)
        {
            printf("synchrnoize!\n");
            // // compute the next expiration and reset the timers based on that
            // long long next_T_start = TI.T_start_n + (count+1) * server_sec;
            // count = 0;
            // as_timespec(next_T_start, &T_star);
            // T_skew_now = TI.T_skew_n;

            // We assume that we are now in a different temperature zone (i.e. 
            // drift differs from past 10 minutes). 
            // The server will provide its next trigger, which should be 
            // within the second window, and we will proceed our next 10 minutes
            // starting here.
//			auto start = std::chrono::steady_clock::now();
			// do something
			/*clock_gettime(CLOCK_REALTIME, &now);
			long long temp = as_nsec(&now);
			std::cout << temp ;*/
            if (synchronize(&TI, 0) == -1) 
            {
                printf("Sychronization error\n");
                exit(1);
            }            
			/*clock_gettime(CLOCK_REALTIME, &now);
			temp = as_nsec(&now);
			std::cout << ", " << temp;*/

//			auto finish = std::chrono::steady_clock::now();
//			std::cout << std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count() << std::endl;
			//TI.T_start_n += server_sec;// * PERIOD;
            as_timespec(TI.T_start_n, &T_trig);
            resetTimer(&cameraTimerID, &T_trig, server_sec*PERIOD);
			T_trig_n = TI.T_start_n;
		//	std::cout << ", "<< TI.T_start_n-temp << std::endl;
            count = 0; // THis doesn't make sense?

            T_drift_n = TI.T_start_n + (drift_period * server_sec) + server_sec/OFFSET;
            as_timespec(T_drift_n, &T_drift);
            resetTimer(&driftTimerID, &T_drift, 0);

            T_sync_n = TI.T_start_n;
            // resetTimer(&driftTimerID, &T_start, server_sec*MIN+(server_sec/4));

            T_skew_now = TI.T_skew_n;
            fSync = 0;
        }

        // set time for 1 min drift computation
        if (fDrift)
        {
            printf("Compute drifts!\n");
//			auto start = std::chrono::steady_clock::now();
            T_skew_prev = T_skew_now;
			//clock_gettime(CLOCK_REALTIME, &now);
			/*long long temp = as_nsec(&now);
			std::cout << temp ;*/
            if (get_skew(&TI) == -1) 
            {
                printf("skew error\n");
                exit(1);
            }                       
			// do something
			/*clock_gettime(CLOCK_REALTIME, &now);
			temp = as_nsec(&now);
			std::cout << ", " << temp;*/
//			auto finish = std::chrono::steady_clock::now();
//			std::cout << std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count() << std::endl;
			T_skew_now = TI.T_skew_n;

            // T_diff is skew difference over a minute that could be locally
            // compensated assuming no change in Temperature over 10 minutes
            // Instead of setting trigger to 1second, we adjust to what is
            // "1 second in server".
            // We also adjust the T_start to the next second that the trigger
            // will start so that the timer is triggered properly.
			double server_period = double((T_skew_now - T_skew_prev)/drift_period + BILLION);
			server_sec = (long long) (double(BILLION)*(double(BILLION) / server_period));
			T_trig_n += (drift_period+1) * server_sec;
			//std::cout << ", "<< T_trig_n << std::endl;
            count = 0;
            as_timespec(T_trig_n, &T_trig);
            resetTimer(&cameraTimerID, &T_trig, server_sec*PERIOD);

            // T_drift_n = T_drift_n + drift_period * server_sec;
            // as_timespec(T_drift_n, &T_drift);
            // resetTimer(&syncTimerID, &T_drift, 0);

            // reset synchronization time with new server second
            T_sync_n = T_sync_n + sync_period * server_sec + server_sec/OFFSET;
            as_timespec(T_sync_n, &T_sync);
            resetTimer(&syncTimerID, &T_sync, 0);

            fDrift = 0;
        }
		// sleep for 1ms
		usleep(100000);
    }
    logger->close();
    // Done Data Acquisition
    // Programmed data acquisition duration elapsed
    //  - Regularly measure depth and temperature until powered off.
    return 0;
}

//std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
//std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
//cout << "Time difference = " << chrono::duration_cast<chrono::microseconds>(end - begin).count() << "[µs]" << endl;
//cout << "Time difference = " << chrono::duration_cast<chrono::nanoseconds> (end - begin).count() << "[ns]" << endl;
//printf("Pressure: %.2f\nAltitude: %.2f\n", k_sensor->pressure(), k_sensor->altitude());
