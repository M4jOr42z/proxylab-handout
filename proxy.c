#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including these long lines in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";
// static const char *accept_hdr = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n";
// static const char *accept_encoding_hdr = "Accept-Encoding: gzip, deflate\r\n";

/* user defined functions */
void doit(int fd);
void clienterror(int fd, char *cause, char *errnum, 
                 char *shortmsg, char *longmsg);
int is_valid(char *buf);
int read_requesthdrs(rio_t *rp, char *reqs);
void parse_uri(char *uri, char *hostname, char *port);
void forward_response(rio_t *rp, int client_fd);

int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    struct sockaddr_in clientaddr;
    socklen_t clientlen = sizeof(clientaddr);

    /* check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    /* ignore SIGPIPE signal */
    signal(SIGPIPE, SIG_IGN);

    listenfd = Open_listenfd(argv[1]);
    while(1) {
        /* listen for incoming connections */
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, 
                     port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        /* service the request accordingly */
        doit(connfd);
        /* close the connection */
        Close(connfd);
    }
    return 0;
}

/*
 * handles one HTTP request/response transaction
 * 1. read and parese client request
 * 2. if valid http request, establish connection to requested server, 
 * request the object on behalf of the client, and forward it to the client
 * 3. if invalid request, send error messgages to the connected client 
 */
void doit(int fd) {
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], 
         version[MAXLINE], hostname[MAXLINE], server_port[MAXLINE];
    char reqs[MAXBUF];
    rio_t rio_client, rio_proxy;
    int proxy_fd; /* descriptor to communicate with server */

    /* read first line of HTTP request */
    Rio_readinitb(&rio_client, fd);
    if (!Rio_readlineb(&rio_client, buf, MAXLINE))
        return;

    /* see if the request is valid */
    printf("read request line: %s", buf);
    if (is_valid(buf)) {
        sscanf(buf, "%s %s %s", method, uri, version);
        if (strcasecmp(method, "GET")) {
            clienterror(fd, method, "501", "Not Implemented", 
                        "Proxy does not implement this method");
            return;
        }
    }
    else {
        clienterror(fd, "", "400", "Bad Requset", 
                    "Proxy does not understand this request");
        return;
    }

    /* parse the uri, and retrieve hostname, port number, and uri for server */
    parse_uri(uri, hostname, server_port);
    printf("uri: %s\n", uri);
    printf("hostname: %s\n", hostname);
    printf("server port: %s\n", server_port);

    /* build the request line */
    sprintf(reqs, "GET %s HTTP/1.0\r\n", uri);

    /* read subsequent request headers and build the requst headers */
    if (!read_requesthdrs(&rio_client, reqs))
        sprintf(reqs, "%sHost: %s\r\n", reqs, hostname);
    sprintf(reqs, "%s%s", reqs, user_agent_hdr);
    sprintf(reqs, "%s%s", reqs, connection_hdr);
    sprintf(reqs, "%s%s", reqs, proxy_connection_hdr);
    sprintf(reqs, "%s\r\n", reqs); /* empty line to end headers */

    /* open a client-end socket and send request to server */
    // printf("hostname: %s\n", hostname);
    // printf("port number: %s\n", server_port);
    proxy_fd = Open_clientfd(hostname, server_port);
    Rio_readinitb(&rio_proxy, proxy_fd);
    Rio_writen(proxy_fd, reqs, strlen(reqs));
    printf("writing the request to server:\n");
    printf("%s", reqs);
    
    /* forward server response to the client and close when finish */
    forward_response(&rio_proxy, fd);
    Close(proxy_fd);

    return;
}

/*
 * determines whether if is a valid http request
 * a form other than <method> <uri> <version> or
 * a method not implemented is considered as invalid.
 * 1 for valid, 0 for not valid
 */
int is_valid(char *buf) {
    char *cp;

    if (*buf != ' ') {
        if ((cp = strchr(buf, ' '))) {
            if (*(cp+1) != ' ' && strchr(cp+1, ' '))
                return 1;
        }
    }
    return 0;
}

/*
 * parses the uri from client, retrieve hostname, port number (if provideded),
 * and update the uri to be sent to the server as request
 */
void parse_uri(char *uri, char *hostname, char *port) {
    char *cp_hd, *cp_ft;

    cp_hd = strstr(uri, "//");
    cp_hd += 2;
    cp_ft = cp_hd;
    /* port is specified */
    if ((cp_ft = strchr(cp_ft, ':'))) {
        *cp_ft = '\0';
        strcpy(hostname, cp_hd);
        cp_hd = cp_ft+1;
        cp_ft = strchr(cp_hd, '/');
        *cp_ft = '\0';
        strcpy(port, cp_hd);
        *uri = '/';
        strcpy(uri+1, cp_ft+1);
    }
    /* use default port 80 */
    else {
        strcpy(port, "80");
        cp_ft = strchr(cp_hd, '/');
        *cp_ft = '\0';
        strcpy(hostname, cp_hd);
        *uri = '/';
        strcpy(uri+1, cp_ft+1);
    }

}

/*
 * reads request headers from client request
 * add the Host header if client provides and return 1
 * otherwise return 0
 * add other adders client provides except those specified
 * in the document
 */
int read_requesthdrs(rio_t *rp, char *reqs) {
    char buf[MAXLINE], hdr_name[MAXLINE], hdr_data[MAXLINE];
    int ret = 0;

    while (1) {
        Rio_readlineb(rp, buf, MAXLINE);
        if (!strcmp(buf, "\r\n"))
            break;
        printf("read hdr: %s\n", buf);
        sscanf(buf, "%s %s", hdr_name, hdr_data);
        if (!strcmp(hdr_name, "Host:")) {   
            sprintf(reqs, "%s%s", reqs, buf);
            ret = 1;
        }
        else if (strcmp(hdr_name, "User-Agent:") && 
                 strcmp(hdr_name, "Connection:") &&
                 strcmp(hdr_name, "Proxy-Connection:")) {
            sprintf(reqs, "%s%s", reqs, buf);
        }
    }
    printf("leave read_requesthdrs\n");
    return ret;

}

/*
 * forwards server's response to client
 */
void forward_response(rio_t *rp, int client_fd) {
    char buf[MAXBUF], hdr_name[MAXLINE], hdr_data[MAXLINE];
    char *buf_p;
    int bytes;
    int content_length = 0;
    char content_type[MAXLINE];

    /* read response line */
    bytes = rio_readlineb(rp, buf, MAXLINE);
    if (bytes <= 0) {
        printf("read response line failed or EOF encountered\n");
        return;
    }
    bytes = rio_writen(client_fd, buf, strlen(buf));
    if (bytes <= 0) {
        printf("forward response line failed\n");
        return;
    }

    /* forward response headers */
    while (1) {
        bytes = rio_readlineb(rp, buf, MAXLINE);
        if (bytes <= 0) {
            printf("read response header failed or EOF encountered\n");
            return;
        }
        printf("read response header: %s", buf);
        bytes = rio_writen(client_fd, buf, strlen(buf));
        if (bytes <= 0) {
            printf("forward response header failed\n");
            return;
        }
        /* empty line, response hdrs finish */
        if (!strcmp(buf, "\r\n")) 
            break;
        /* not empty line, extract content type and content length */
        sscanf(buf, "%s %s", hdr_name, hdr_data);
        // printf("hdr_name: %s\n", hdr_name);
        // printf("hdr_data: %s\n", hdr_data);
        if (!strcmp(hdr_name, "Content-Type:")
            || !strcmp(hdr_name, "Content-type:"))
            strcpy(content_type, hdr_data);
        if (!strcmp(hdr_name, "Content-Length:")
            || !strcmp(hdr_name, "Content-length:"))
            content_length = atoi(hdr_data);
    }
    printf("content type: %s\n", content_type);
    printf("content length: %d\n", content_length);

    /* forward response body */
    printf("forward response body begin...\n");
    if (content_length) {
        buf_p = malloc(content_length);
        bytes = rio_readnb(rp, buf_p, content_length);
        if (bytes <= 0) {
            printf("read response body failed or EOF encoutnered\n");
            free(buf_p);
            return;
        } 
        bytes = rio_writen(client_fd, buf_p, content_length);
        if (bytes <=0) {
            printf("write response body failed\n");
            free(buf_p);
            return;
        }
        printf("write %d bytes to client\n", bytes);
        free(buf_p);   
    }
    else
        printf("content length not found!");
}

/*
 * returns an error message to the client when request is invalid
 */
void clienterror(int fd, char *cause, char *errnum, 
                 char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    /* build HTTP response body */
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Proxy</em>\r\n", body);

    /* print the HTTP response */
    sprintf(buf,  "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));  
} 
