#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <curl/curl.h>

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define CSV_DIRECTORY           "http://nirsoft.net/countryip/"
#define CSV_DOWNLOAD_TIMEOUT    5
#define CSV_EXTENSION           ".csv"
#define CSV_LINEBUF_SIZE        1024
#define LIBCURL_LEGACY          true // set this to true for Ubuntu <= 13.04
#define LOG                     true
#define DEFAULT_NUM_THREADS     256
#define RANDOM_STATEBUF_SIZE    64
#define REALM_HEADER            "WWW-Authenticate: Basic realm=\""
#define SCAN_MAX_REDIRS         3
#define SCAN_TIMEOUT            3

typedef struct {
  char startAddress[16];
  size_t numAddresses;
  size_t numThreads;
  char countryCode[3];
  bool random;
} args_t;
