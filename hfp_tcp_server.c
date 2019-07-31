//
//  hfp_tcp_server.c
//
//  Serves IQ data using the rtl_tcp protocol
//    from an Airspy HF+
//    on iPv6 port 1234
//
#define VERSION "v.1.2.114" // beta 02
//   v.1.2.114 2019-07-31  rhn 
//   v.1.2.112 2019-07-30  0am barry@medoff.com
//   v.1.2.111 2019-05-05  2pm  rhn
//   v.1.2.109 2019.05.04 10pm barry@medoff.com
//   Copyright 2017,2019 Ronald H Nicholson Jr. All Rights Reserved.
//   re-distribution under the BSD 2 clause license permitted
//
//   macOS : clang hfp_tcp_server.c -o hfp_tcp -lm -llibairspyhf
//   					// libairspyhf.1.6.8.dylib
//   pi :    cc -std=c99 -lm -lairspyhf -O2 -o hfp_tcp hfp_tcp_server.c
//
//   requires these 2 files to compile
//     airspyhf.h
//     libairspyhf.1.6.8.dylib or /usr/local/lib/libairspyhf.so.1.6.8
//   from libairspyhf at https://github.com/airspy/airspyhf
//

#define SOCKET_READ_TIMEOUT_SEC ( 10.0 * 60.0 )
#define SAMPLE_BITS     ( 8)    // default to match rtl_tcp
// #define SAMPLE_BITS  (16)    // default to match rsp_tcp
// #define SAMPLE_BITS  (32)    // HF+ capable of float32 IQ data
#define GAIN8           (64.0)  // default gain
#define PORT            (1234)  // default port

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifndef _UNISTD_H_
int usleep(unsigned long int usec);
#endif

#include <sys/time.h>

#include "airspyhf.h"

void *connection_handler(void);
int sendcallback(airspyhf_transfer_t *context);
static void sighandler(int signum);

uint64_t            serialnum   =  0;
airspyhf_device_t   *device     =  NULL;
airspyhf_transfer_t context;

int             sendErrorFlag   =  0;
int             sampleBits      =  SAMPLE_BITS;
int             sampleRates     =  1;
static long int totalSamples    =  0;
long        sampRate            =  768000;
long        previousSRate       = -1;
float        gain0              =  GAIN8;
int        gClientSocketID      = -1;

static int    listen_sockfd;
struct sigaction    sigact, sigign;
static volatile int     do_exit =  0;
float        acc_r              =  0.0;    // accumulated rounding
float        sMax               =  0.0;    // for debug
float        sMin               =  0.0;

char UsageString[]
    = "Usage:    [-p listen port (default: 1234)]\n          [-b 16]";

int main(int argc, char *argv[]) {

    struct sockaddr_in6 serv_addr ;
    char client_addr_ipv6[100];
    int portno     =  PORT;     //
    char *ipaddr =  NULL;       // "127.0.0.1"
    int n;

    if (argc > 1) {
    if ((argc % 2) != 1) {
            printf("%s\n", UsageString);
            exit(0);
        }
        for (int arg=3; arg<=argc; arg+=2) {
        if (strcmp(argv[arg-2], "-p")==0) {
                portno = atoi(argv[arg-1]);
                if (portno == 0) {
                    printf("invalid port number entry %s\n", argv[arg-1]);
                    exit(0);
                }
            } else if (strcmp(argv[arg-2], "-b")==0) {
                if (strcmp(argv[arg-1],"16")==0) {
                    sampleBits = 16;
                } else if (strcmp(argv[arg-1],"8")==0) {
                    sampleBits =  8;
                } else {
                    printf("%s\n", UsageString);
                    exit(0);
                }
            } else if (strcmp(argv[arg-2], "-a")==0) {
        ipaddr = argv[arg-1];        // unused
            } else {
                printf("%s\n", UsageString);
                exit(0);
            }
        }
    }

    printf("\nhfp_tcp Version %s\n\n", VERSION);
    printf("Serving %d-bit samples on port %d\n", sampleBits, portno);

    uint64_t serials[4] = { 0L,0L,0L,0L };
    int count = 2;
    n = airspyhf_list_devices(&serials[0], count);
    printf("hf+ devices = %d\n", n);
    if (n == 0L) { exit(-1); }
    printf("hf+ serial# = ");
    uint64_t t = serials[0];
    printf("%" PRIu64 "\n", t);
    serialnum = t;
    if (serialnum == 0L) { exit(-1); }

    sigact.sa_handler = sighandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT,  &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
#ifdef __APPLE__
    signal(SIGPIPE, SIG_IGN);
#else
    sigign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sigign, NULL);
#endif

    n = airspyhf_open_sn(&device, serialnum);
    printf("hf+ open status = %d\n", n);
    if ((n < 0) || (device == NULL)) { exit(-1); }

    airspyhf_lib_version_t version;
    airspyhf_lib_version(&version);
    printf("\nlibairspyhf   %" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
           version.major_version, version.minor_version, version.revision);

    char versionString[64];
    uint8_t versionLength = 64;

    bzero((char *)&versionString[0], 64);

    n = airspyhf_version_string_read(device, &versionString[0], versionLength);
    if (n == AIRSPYHF_ERROR) {
    printf("Error reading version string");
    exit(-1);
    }
    printf("hf+ firmware %s\n\n", versionString);

    uint32_t sr_buffer[100];
    airspyhf_get_samplerates(device, sr_buffer, 0);
    uint32_t sr_len = sr_buffer[0];
    printf("number of supported sample rates: %d \n", sr_len);
    if (sr_len > 0 && sr_len < 100) {
      sampleRates     =  sr_len;
      airspyhf_get_samplerates(device, sr_buffer, sr_len);
      printf("supported sample rates: ");
        for (int i=0; i<sr_len; i++) {
          printf("%d ", sr_buffer[i]);
        }
        printf(" \n\n");
    }

    int sampRate = 768000;
    n = airspyhf_set_samplerate(device, sampRate);
    printf("set rate status = %d %d\n", sampRate, n);
    previousSRate = sampRate;
    long int f0 = 162450000;
    n = airspyhf_set_freq(device, f0);
    printf("set f0 status = %ld %d\n", f0, n);

    printf("\nhfp_tcp IPv6 server started on port %d\n", portno);

    listen_sockfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_sockfd < 0) {
        printf("ERROR opening socket");
        return(-1);
    }

    struct linger ling = {1,0};
    int rr = 1;
    setsockopt(listen_sockfd, SOL_SOCKET, SO_REUSEADDR,
            (char *)&rr, sizeof(int));
    setsockopt(listen_sockfd, SOL_SOCKET, SO_LINGER,
            (char *)&ling, sizeof(ling));

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin6_flowinfo = 0;
    serv_addr.sin6_family = AF_INET6;
    serv_addr.sin6_addr = in6addr_any;
    serv_addr.sin6_port = htons(portno);

    // Sockets Layer Call: bind()
    if (bind( listen_sockfd, (struct sockaddr *)&serv_addr,
             sizeof(serv_addr) ) < 0) {
        printf("ERROR on bind to listen\n");
        return(-1);
    }

    listen(listen_sockfd, 5);
    fprintf(stdout, "listening for socket connection \n");

    while (1) {

        // accept a connection

        struct sockaddr_in6 cli_addr;
        socklen_t claddrlen = sizeof(cli_addr);
        gClientSocketID = accept( listen_sockfd,
                                 (struct sockaddr *) &cli_addr,
                                 &claddrlen );
        if (gClientSocketID < 0) {
            printf("ERROR on accept\n");
            break;
        }

        inet_ntop(AF_INET6, &(cli_addr.sin6_addr), client_addr_ipv6, 100);
        printf("\nConnected to client with IP address: %s\n",
               client_addr_ipv6);

        connection_handler();

    }

    n = airspyhf_close(device);
    printf("hf+ close status = %d\n", n);

    fflush(stdout);
    return 0;
}  //  main

static void sighandler(int signum)
{
        fprintf(stderr, "Signal caught, exiting!\n");
        fflush(stderr);
        close(listen_sockfd);
        if (gClientSocketID != 0) {
            close(gClientSocketID);
            gClientSocketID = -1;
        }
        if (device != NULL) {
            airspyhf_close(device);
            device = NULL;
        }
    exit(-1);
        do_exit = 1;
}

void *connection_handler()
{
    char buffer[256];
    int n = 0;
    int m = 0;

    if (do_exit != 0) { return(NULL); }

    m = airspyhf_is_streaming(device);
    if (m > 0) {    // stop before restarting
        printf("hf+ is running = %d\n", m);
        m = airspyhf_stop(device);
        printf("hf+ stop status = %d\n", m);
        usleep(250L * 1000L);
    }

    acc_r      =  0.0;
    totalSamples  =  0;
    sendErrorFlag =  0;
    m = airspyhf_start(device, &sendcallback, &context);
    printf("hf+ start status = %d\n", m);
    if (m < 0) { exit(-1); }
    usleep(250L * 1000L);

    // set a timeout so receive call won't block forever
    struct timeval timeout;
    timeout.tv_sec = SOCKET_READ_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    //setsockopt( gClientSocketID, SOL_SOCKET, SO_RCVTIMEO,
    //           &timeout, sizeof(timeout) );

    if (1) {        // 16 or 12-byte rtl_tcp header
        int sz = 16;
        if (sampleBits == 8) { sz = 12; }
        // HFP0 16
        char header[16] = { 0x48,0x46,0x50,0x30, 
	    0x30,0x30,0x30+sampleRates,0x30+sampleBits,
            0,0,0,1, 0,0,0,2 };
#ifdef __APPLE__
        n = send(gClientSocketID, header, sz, 0);
#else
        n = send(gClientSocketID, header, sz, MSG_NOSIGNAL);
#endif
    }

    n = 1;
    while ((n > 0) && (sendErrorFlag == 0)) {
        int i, j, m;
        // receive 5 byte commands (or a multiple thereof)
        memset(buffer,0, 256);
        n = recv(gClientSocketID, buffer, 255, 0);
        if ((n <= 0) || (sendErrorFlag != 0)) {
            if (airspyhf_is_streaming(device)) {
                m = airspyhf_stop(device);
            }
            close(gClientSocketID);
            fprintf(stdout,"00 \n");
            fprintf(stdout, "hf+ stop status = %d\n", m);
            fflush(stdout);
            break;
        }
        if (n > 0) {
            int msg1 = buffer[0];
            if (msg1 != 4) {
                for (i=0; i < n; i++) {
                    fprintf(stdout, "%02x ", (0x00ff & buffer[i]));
                }
                if (n > 0) { fprintf(stdout, "\n"); }
            }
            for (i=0; i < n; i+=5) {
                // decode 5 byte rtl_tcp command messages
                int msg  = buffer[i];
                int data = 0;
                for (j=1;j<5;j++) {
                    data = 256 * data + (0x00ff & buffer[i+j]);
                }

                if (msg == 1) {    // set frequency
                    int f0 = data;
                    fprintf(stdout, "setting frequency to: %d\n", f0);
                    m = airspyhf_set_freq(device, f0);
                    printf("set frequency status = %d\n", m);
                }
                if (msg == 2) {    // set sample rate
                    int r = data;
                    if (r != previousSRate) {
		        int restartflag = 0;
                        fprintf(stdout, "setting samplerate to: %d\n", r);
                        sampRate = r;
    			m = airspyhf_is_streaming(device);
    			if (m > 0) {    // stop before restarting
        		    m = airspyhf_stop(device);
		            restartflag = 1;
			    usleep(50L * 1000L);
			}
                        m = airspyhf_set_samplerate(device, sampRate);
                        printf("set samplerate status = %d\n", m);
                        previousSRate = r;
		        if (restartflag == 1) {
			    usleep(50L * 1000L);
                            m = airspyhf_start(device, 
			    		&sendcallback, &context);
                            fprintf(stdout, "hf+ start status = %d\n", m);
                            m = airspyhf_is_streaming(device);
                            fprintf(stdout, "hf+ is running = %d\n", m);
                            fflush(stdout);
			}
                    }
                }
                if (msg == 3) {    // other
                    fprintf(stdout, "message = %d, data = %d\n", msg, data);
                }
                if (msg == 4) {            // gain
                    if (   (sampleBits ==  8)
            || (sampleBits == 16) ) {
                        // set gain ?
                        float g1 = data; // data : in 10th dB's
                        float g2 = 0.1 * (float)(data); // undo 10ths
                        fprintf(stdout, "setting gain to: %f dB\n", g2);
                        float g4 = g2 - 12.0; // ad hoc offset
                        float g5 = pow(10.0, 0.1 * g4); // convert from dB
                        gain0 = GAIN8 * g5;        // 64.0 = nominal
                        msg1 = msg;
                        float  g8  =  gain0; // GAIN8;
                        fprintf(stdout, "8b  gain multiplier = %f\n", g8);
                float  g16 =   64.0 * gain0; // GAIN16;
                        fprintf(stdout, "16b gain multiplier = %f\n", g16);
            }
                }
                if (msg > 4) {    // other
                    fprintf(stdout, "message = %d, data = %d\n", msg, data);
                }
            }
            if (msg1 != 4) {
                m = airspyhf_is_streaming(device);
                printf("hf+ is running = %d\n", m);
                if (m == 0) {    // restart if command stops things
                    sendErrorFlag =  0;
                    m = airspyhf_start(device, &sendcallback, &context);
                    fprintf(stdout, "hf+ start status = %d\n", m);
                    m = airspyhf_is_streaming(device);
                    fprintf(stdout, "hf+ is running = %d\n", m);
                    fflush(stdout);
                }
            }
        }
        if (n < 0) {
            fprintf(stdout, "read socket timeout %d \n", n);
            fflush(stdout);
        }
        // loop until error (socket close) or timeout
    } ;

    m = airspyhf_is_streaming(device);
    printf("hf+ is running = %d\n", m);
    if (m) {
        m = airspyhf_stop(device);
        printf("hf+ stop status = %d\n", m);
    }

    close(gClientSocketID);
    return(NULL);
} // connection_handler()

int sendblockcount = 0;
uint8_t tmpBuf[4*32768];

typedef union
{
    uint32_t i;
    float    f;
} Float32_t;

float rand_float_co()
{
    Float32_t x;
    x.i = 0x3f800000 | (rand() & 0x007fffff);
    return(x.f - 1.0f);
}


int sendcallback(airspyhf_transfer_t *context)
{
    float  *p =  (float *)(context->samples);
    int    n  =  context->sample_count;
    int       sz ;

    if (do_exit != 0) { return(-1); }
    //
    if ((sendblockcount % 1000) == 0) {
        fprintf(stdout,"+"); fflush(stdout);
    }
    //
    if (p != NULL && n > 0) {
        // fwrite(p, 8, n, file);
        char    *dataBuffer    =  (char *)p;
        int    k        =  0;
        if (sampleBits ==  8) {
            float  g8  =  gain0; // GAIN8;
            // gain is typically 64.0
            // should be 128.0 or 2X larger, so 1-bit missing
            float rnd0A = rand_float_co();
            float rnd0B = rand_float_co();
            for (int i=0; i<2*n; i++) {
                float x;
                Float32_t x1;          // for debug hex print
                x    = p[i];
                float y = g8 * x;
                // add triangular noise
                // for noise filtered rounding
                float rnd1 = rand_float_co(); // noise with pdf [0..1)
                float r = rnd1 - (((i&1)==1) ? rnd0A : rnd0B);
                y = y + r;
                float ry = roundf(y);
                acc_r += (y - ry);     // for future noise filtering
                k = (int)ry;
                tmpBuf[i] = k + 128;
                if ((i&1) == 1) {      // round I
                    rnd0A = rnd1;      // save for next iteration
                } else {               // round Q
                    rnd0B = rnd1;      // save for next iteration
                }
            }
            // previous rounding
            /*
            for (int i=0; i<2*n; i++) {
                float x = g8 * p[i];
                int   k = (int)roundf(x);
                tmpBuf[i] = k + 128;  // 8-bit unsigned DC offset
            }
            */
            dataBuffer = (char *)(&tmpBuf[0]);
            sz = 2 * n;
        } else if (sampleBits == 16) {
            int16_t *tmp16ptr = (int16_t *)&tmpBuf[0];
            float  g16  =   64.0 * gain0; // GAIN16;
            // gain is typically 64.0 * 64.0 = 4096.0
            // should be 32768.0 or 8X larger, so 3-bits missing
            for (int i=0; i<2*n; i++) {
                float x = g16 * p[i];
                int   k = (int)roundf(x);
                tmp16ptr[i] = k;
            }
            dataBuffer = (char *)(&tmpBuf[0]);
            sz = 4 * n;
        } else {
            sz = 8 * n;    // two 32-bit floats for IQ == 8 bytes
        }
        int send_sockfd = gClientSocketID ;
        if (do_exit != 0) { return(-1); }
#ifdef __APPLE__
        k = send(send_sockfd, dataBuffer, sz, 0);
#else
        k = send(send_sockfd, dataBuffer, sz, MSG_NOSIGNAL);
#endif
        if (do_exit != 0) { return(-1); }
        if (k <= 0) { sendErrorFlag = -1; }
        totalSamples += n;
    }
    sendblockcount += 1;
    return(0);
}

// eof
