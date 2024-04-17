#include <stdio.h>
#include "csapp.h"
#include "sbuf.h"
#include "cache.h"


/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define WEB_PREFIX "http://"
#define NTHREADS  4
#define SBUFSIZE  16
#define CR "\r\n"

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

void handleRequest(int);

void clientError(int, char *, char *, char *, char *);

int readAndFormatRequestHeader(rio_t *, char *, char *, char *, char *, char *, char *, char *);

void replaceHTTPVersion(char *);

void parseLine(char *, char *, char *, char *, char *, char *, char *);

void *thread(void *vargp);

void readAndWriteResponse(int, rio_t *, char *uri);

static sbuf_t sbuf; /* Shared buffer of connected descriptors */

int main(int argc, char **argv) {
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);

    sbuf_init(&sbuf, SBUFSIZE);
    for (int i = 0; i < NTHREADS; i++)  /* Create worker threads */
        Pthread_create(&tid, NULL, thread, NULL);

    /* initialize the cache */
    initializeCache(&cache);

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *) &clientaddr, &clientlen); //line:netp:tiny:accept
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE,
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        sbuf_insert(&sbuf, connfd); /* Insert connfd in buffer */
    }
}

void *thread(void *vargp) {
    //threads should run in detached mode to avoid memory leaks.
    Pthread_detach(pthread_self());
    while (1) {
        int connfd = sbuf_remove(&sbuf); /* Remove connfd from buffer */
        handleRequest(connfd);              /* Service client */
        Close(connfd);
    }
}

void handleRequest(int fd) {
    /* the argument used to parse the first line of http request */
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE], fileName[MAXLINE];

    /* client request header and body */
    char clientRequest[MAXLINE];

    /* request header */
    char host[MAXLINE], port[MAXLINE];

    /* IO for proxy--client,  proxy-server */
    rio_t rio, rioTiny;

    /** step1: read request from client */
    Rio_readinitb(&rio, fd);
    if (Rio_readlineb(&rio, buf, MAXLINE) == 0) {
        //the request has the empty space
        printf("empty request\n");
        return;
    }

    /* if start with 1.1, we change it to 1.0 */
    replaceHTTPVersion(buf);

    /* parse one line request line into several parts */
    parseLine(buf, host, port, method, uri, version, fileName);

    /** step2: determine if it is a valid request */
    if (strcasecmp(method, "GET") != 0) {
        clientError(fd, method, "501", "Not Implemented", "Tiny Does not implement this method");
        return;
    }

    int rv1 = readAndFormatRequestHeader(&rio, clientRequest, host, port, method, uri, version, fileName);
    if (rv1 == 0) { // bad request, ignore it
        return;
    }

    obj_t *rv = readItem(uri, fd);
    if (rv != NULL) { // we send it using cache
        printf("======we are using cache to send the response back ====\n");

        Rio_writen(fd, rv->respHeader, rv->respHeaderLen);
        Rio_writen(fd, CR, strlen(CR));
        Rio_writen(fd, rv->respBody, rv->respBodyLen);
        return;
    }

    printf("========= we have formatted the reqeust into ---------\n");
    printf("%s", clientRequest);

    /** step3: establish own connection with tiny server
     *         and forward the request to the client
     * localhost:1025
     * */
    char hostName[100];
    char *colon = strstr(host, ":");
    strncpy(hostName, host, colon - host);
//    printf("host is %s\n", hostName);
//    printf("port is %s\n", port);
    int clientfd = Open_clientfd(hostName, port);

    Rio_readinitb(&rioTiny, clientfd);
    Rio_writen(rioTiny.rio_fd, clientRequest, strlen(clientRequest));

    /** step4: read the response from tiny and send it to the client */
//    printf("---prepare to get the response---- \n");
//    char tinyResponse[MAXLINE];
//    int n;
//    while ((n = Rio_readnb(&rioTiny, tinyResponse, MAXLINE)) != 0) {
//        Rio_writen(fd, tinyResponse, n);
//    }

    readAndWriteResponse(fd, &rioTiny, uri);
}

void readAndWriteResponse(int fd, rio_t *rioTiny, char *uri) {
    char tinyResponse[MAXLINE];
    int n, totalBytes = 0;
    //new response
    obj_t *obj = Malloc(sizeof(*obj));
    obj->flag = '0';
    strcpy(obj->uri, uri);
    *obj->respHeader = 0;
    *obj->respBody = 0;

    printf("obj->url == %s\n", obj->uri);

    while ((n = rio_readlineb(rioTiny, tinyResponse, MAXLINE)) != 0) {
        Rio_writen(fd, tinyResponse, n);

        // https://developer.mozilla.org/en-US/docs/Web/HTTP/Messages
        if (strcmp(tinyResponse, "\r\n") == 0) // prepare for body part
            break;

        strcat(obj->respHeader, tinyResponse);
        totalBytes += n;
    }

    obj->respHeaderLen = totalBytes;
    totalBytes = 0;

    while ((n = rio_readlineb(rioTiny, tinyResponse, MAXLINE)) != 0) {
        Rio_writen(fd, tinyResponse, n);
        totalBytes += n;
        strcat(obj->respBody, tinyResponse);
    }

    obj->respBodyLen = totalBytes;
    //check if the cache is too large
    if (totalBytes >= MAX_OBJECT_SIZE) {
        Free(obj);
        return;
    }

//    printf("In reading Responsse, we have cache the follwing item\n");
//    printf("======= response header ========\n");
//    printf("%s", obj->respHeader);
//    printf("======= response body ==========\n");
//    printf("%s", obj->respBody);

    /* try to read current capacity and write into it */
    writeToCache(obj);
//    printf("=======writng this object in to cache=========\n");
}


void replaceHTTPVersion(char *buf) {
    char *pos = NULL;
    if ((pos = strstr(buf, "HTTP/1.1")) != NULL) {
        buf[pos - buf + strlen("HTTP/1.1") - 1] = '0';
    }
}

/**
 * @param: client request is like this
 *          GET http://www.cmu.edu/hub/index.html HTTP/1.1
 * @function: we will parse that into differnt parameters
 */
void parseLine(char *buf, char *host, char *port, char *method, char *uri, char *version, char *fileName) {
    sscanf(buf, "%s %s %s", method, uri, version);
    //method = "GET", uri = "http://localhost:15213/home.html", version = "HTTP1.0"

    char *hostp = strstr(uri, WEB_PREFIX) + strlen(WEB_PREFIX);
    char *slash = strstr(hostp, "/");
    char *colon = strstr(hostp, ":");
    //get host name
    strncpy(host, hostp, slash - hostp);
    //get port number
    strncpy(port, colon + 1, slash - colon - 1);
    //get file name
    strcpy(fileName, slash);
}

void clientError(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE];

    /* Print the HTTP response headers */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    Rio_writen(fd, buf, strlen(buf));

    /* Print the HTTP response body */
    sprintf(buf, "<html><title>Tiny Error</title>");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=""ffffff"">\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>The Tiny Web server</em>\r\n");
    Rio_writen(fd, buf, strlen(buf));
}

int readAndFormatRequestHeader(rio_t *rio, char *clientRequest, char *Host, char *port,
                               char *method, char *uri, char *version, char *fileName) {
    int UserAgent = 0, Connection = 0, ProxyConnection = 0, HostInfo = 0;
    char buf[MAXLINE / 2];
    int n;

    /* 1. add GET HOSTNAME HTTP/1.0 to header && Host Info */
    sprintf(clientRequest, "GET %s HTTP/1.0\r\n", fileName);

    n = Rio_readlineb(rio, buf, MAXLINE);
    printf("receive buf %s\n", buf);
    printf("n == %d\n", n);
    char *findp;
    while (strcmp("\r\n", buf) != 0 && n != 0) {
        strcat(clientRequest, buf);
        printf("receive buf %s\n", buf);

        if ((findp = strstr(buf, "User-Agent:")) != NULL) {
            UserAgent = 1;
        } else if ((findp = strstr(buf, "Proxy-Connection:")) != NULL) {
            ProxyConnection = 1;
        } else if ((findp = strstr(buf, "Connection:")) != NULL) {
            Connection = 1;
        } else if ((findp = strstr(buf, "Host:")) != NULL) {
            HostInfo = 1;
        }

        n = Rio_readlineb(rio, buf, MAXLINE);
    }

    if (n == 0) {
        return 0;
    }

    if (HostInfo == 0) {
        sprintf(buf, "Host: %s\r\n", Host);
        strcat(clientRequest, buf);
    }

    /** append User-Agent */
    if (UserAgent == 0) {
        strcat(clientRequest, user_agent_hdr);
    }

    /** append Connection */
    if (Connection == 0) {
        sprintf(buf, "Connection: close\r\n");
        strcat(clientRequest, buf);
    }

    /** append Proxy-Connection */
    if (ProxyConnection == 0) {
        sprintf(buf, "Proxy-Connection: close\r\n");
        strcat(clientRequest, buf);
    }

    /* add terminator for request */
    strcat(clientRequest, "\r\n");
    return 1;
}


