#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "postgres.h"
#include "regex/regex.h"

#include "plc_docker_api.h"
#include "plc_configuration.h"

/* Templates for Docker API communication */

// Docker API version used in all the API calls
// v1.23 is available in Docker v1.11.x+
static char *plc_docker_api_version = "v1.22";

// Default location of the Docker API unix socket
static char *plc_docker_socket = "/var/run/docker.sock";

// Post message template. Used by "create" call
static char *plc_docker_post_message_json =
        "POST %s HTTP/1.1\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: application/json\r\n\r\n"
        "%s";

// Post message with text content. Used by "start" and "wait" calls
static char *plc_docker_post_message_text =
        "POST %s HTTP/1.1\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "%s";

static char *plc_docker_get_message =
        "GET /%s/containers/%s/json HTTP/1.1\r\n\r\n";

// JSON body of the "create" call with container creation parameters
static char *plc_docker_create_request =
        "{\n"
        "    \"AttachStdin\": false,\n"
        "    \"AttachStdout\": false,\n"
        "    \"AttachStderr\": false,\n"
        "    \"Tty\": false,\n"
        "    \"Cmd\": [\"%s\"],\n"
        "    \"Image\": \"%s\",\n"
        "    \"DisableNetwork\": false,\n"
        "    \"HostConfig\": {\n"
        "        \"Binds\": [%s],\n"
        "        \"Memory\": %lld,\n"
        "        \"PublishAllPorts\": true\n"
        "    }\n"
        "}\n";

// Regular expression for parsing "create" call JSON response
static char *plc_docker_containerid_regex =
        "\\{\\s*\"[Ii][Dd]\\s*\"\\:\\s*\"(\\w+)\"\\s*,\\s*\"[Ww]arnings\"\\s*\\:([^\\}]*)\\s*\\}";

// Regular expression for parsing container inspection JSON response
static char *plc_docker_port_regex =
        "\"8080\\/tcp\"\\s*\\:\\s*\\[.*\"HostPort\"\\s*\\:\\s*\"([0-9]*)\".*\\]";

// Request for deleting the container
static char *plc_docker_delete_request =
        "DELETE /%s/containers/%s HTTP/1.1\r\n\r\n";

/* End of templates */

/* Static functions of the Docker API module */
static char *get_sharing_options(plcContainer *cont);
static int docker_parse_container_id(char* response, char **name);
static int docker_parse_port_mapping(char* response, int *port);
static int get_content_length(char *msg, int *len);
static int send_message(int sockfd, char *message);
static int recv_message(int sockfd, char *buf, int buflen);
static int recv_port_mapping(int sockfd, char *buf, int buflen, int *port);
static int docker_call(int sockfd, char *request, char **response, int silent);
static int plc_docker_container_command(int sockfd, char *name, const char *cmd, int silent);

static char *get_sharing_options(plcContainer *cont) {
    char *res = NULL;

    if (cont->nSharedDirs > 0) {
        char **volumes = NULL;
        int totallen = 0;
        char *pos;
        int i;

        volumes = palloc(cont->nSharedDirs * sizeof(char*));
        for (i = 0; i < cont->nSharedDirs; i++) {
            volumes[i] = palloc(10 + strlen(cont->sharedDirs[i].host) +
                                 strlen(cont->sharedDirs[i].container));
            if (cont->sharedDirs[i].mode == PLC_ACCESS_READONLY) {
                sprintf(volumes[i], "\"%s:%s:ro\"", cont->sharedDirs[i].host,
                        cont->sharedDirs[i].container);
            } else if (cont->sharedDirs[i].mode == PLC_ACCESS_READWRITE) {
                sprintf(volumes[i], "\"%s:%s:rw\"", cont->sharedDirs[i].host,
                        cont->sharedDirs[i].container);
            } else {
                elog(ERROR, "Cannot determine directory sharing mode");
            }
            totallen += strlen(volumes[i]);
        }

        res = palloc(totallen + 2*cont->nSharedDirs);
        pos = res;
        for (i = 0; i < cont->nSharedDirs; i++) {
            memcpy(pos, volumes[i], strlen(volumes[i]));
            pos += strlen(volumes[i]);
            *pos = ' ';
            pos += 1;
            pfree(volumes[i]);
        }
        *pos = '\0';
        pfree(volumes);
    } else {
        res = palloc(1);
        res[0] = '\0';
    }
    return res;
}

static int docker_parse_container_id(char* response, char **name) {
    regex_t     preg;
    regmatch_t  pmatch[3];
    int         res = 0;
    int         len = 0;
    int         wmasklen, masklen;
    pg_wchar   *mask;
    int         wdatalen, datalen;
    pg_wchar   *data;

    masklen = strlen(plc_docker_containerid_regex);
    mask = (pg_wchar *) palloc((masklen + 1) * sizeof(pg_wchar));
    wmasklen = pg_mb2wchar_with_len(plc_docker_containerid_regex, mask, masklen);

    res = pg_regcomp(&preg, mask, wmasklen, REG_ADVANCED);
    pfree(mask);
    if (res < 0) {
        elog(ERROR, "Cannot compile Postgres regular expression: '%s'", strerror(errno));
        return -1;
    }

    datalen = strlen(response);
    data = (pg_wchar *) palloc((datalen + 1) * sizeof(pg_wchar));
    wdatalen = pg_mb2wchar_with_len(response, data, datalen);

    res = pg_regexec(&preg,
                     data,
                     datalen,
                     0,
                     NULL,
                     3,
                     pmatch,
                     0);
    pfree(data);
    if(res == REG_NOMATCH) {
        elog(ERROR, "Docker API response does not match regular expression");
        return -1;
    }

    if (pmatch[1].rm_so == -1) {
        elog(ERROR, "Postgres regex failed to extract created container name from Docker API response");
        return -1;
    }

    len = pmatch[1].rm_eo - pmatch[1].rm_so;
    *name = palloc(len + 1);
    memcpy(*name, response + pmatch[1].rm_so, len);
    (*name)[len] = '\0';

    if (pmatch[2].rm_so != -1 && pmatch[2].rm_eo - pmatch[2].rm_so > 10) {
        char *err = NULL;
        len = pmatch[2].rm_eo - pmatch[2].rm_so;
        err = palloc(len+1);
        memcpy(err, response + pmatch[2].rm_so, len);
        err[len] = '\0';
        elog(WARNING, "Docker API 'create' call returned warning message: '%s'", err);
    }

    pg_regfree(&preg);
    return 0;
}

static int docker_parse_port_mapping(char* response, int *port) {
    regex_t     preg;
    regmatch_t  pmatch[2];
    int         res = 0;
    int         len = 0;
    int         wmasklen, masklen;
    pg_wchar   *mask;
    int         wdatalen, datalen;
    pg_wchar   *data;
    char       *portstr;

    masklen = strlen(plc_docker_port_regex);
    mask = (pg_wchar *) palloc((masklen + 1) * sizeof(pg_wchar));
    wmasklen = pg_mb2wchar_with_len(plc_docker_port_regex, mask, masklen);

    res = pg_regcomp(&preg, mask, wmasklen, REG_ADVANCED);
    pfree(mask);
    if (res < 0) {
        elog(ERROR, "Cannot compile Postgres regular expression: '%s'", strerror(errno));
        return -1;
    }

    datalen = strlen(response);
    data = (pg_wchar *) palloc((datalen + 1) * sizeof(pg_wchar));
    wdatalen = pg_mb2wchar_with_len(response, data, datalen);

    res = pg_regexec(&preg,
                     data,
                     datalen,
                     0,
                     NULL,
                     2,
                     pmatch,
                     0);
    pfree(data);
    if(res == REG_NOMATCH) {
        return -1;
    }

    if (pmatch[1].rm_so == -1) {
        return -1;
    }

    len = pmatch[1].rm_eo - pmatch[1].rm_so;
    portstr = malloc(len + 1);
    memcpy(portstr, response + pmatch[1].rm_so, len);
    portstr[len] = '\0';

    *port = strtol(portstr, NULL, 10);
    return 0;
}

static int get_content_length(char *msg, int *len) {
    char *contentlen = NULL;
    char *content = NULL;

    contentlen = strstr(msg, "No Content");
    if (contentlen) {
        *len = 1;
        return 0;
    }

    contentlen = strstr(msg, "Content-Length:");
    if (!contentlen) {
        return -1;
    }

    content = strstr(msg, "\r\n\r\n");
    if (!content) {
        return -1;
    }

    contentlen += strlen("Content-Length:") + 1;
    *len = strtol(contentlen, NULL, 10);
    *len += content - msg + 4;

    return 0;
}

static int send_message(int sockfd, char *message) {
    int sent = 0;
    int len = strlen(message);

    while (sent < len) {
        int bytes = 0;

        bytes = send(sockfd, message+sent, len-sent, 0);
        if (bytes < 0) {
            elog(ERROR, "Error writing message to the Docker API socket: '%s'", strerror(errno));
            return -1;
        }

        sent += bytes;
    }

    return 0;
}

static int recv_message(int sockfd, char *buf, int buflen) {
    int received = 0;
    int len = 0;

    memset(buf, 0, buflen);
    while (len == 0 || received < len) {
        int bytes = 0;

        bytes = recv(sockfd, buf + received, buflen - received, 0);
        if (bytes < 0) {
            elog(ERROR, "Error reading response from Docker API socket");
            return -1;
        }
        received += bytes;

        if (len == 0) {
            get_content_length(buf, &len);
        }
    }

    return 0;
}

static int recv_port_mapping(int sockfd, char *buf, int buflen, int *port) {
    int received = 0;

    memset(buf, 0, buflen);
    while (received < buflen) {
        int bytes = 0;

        bytes = recv(sockfd, buf + received, buflen - received, 0);
        if (bytes < 0) {
            elog(ERROR, "Error reading response from Docker API socket");
            return -1;
        }
        received += bytes;

        if (strncmp(buf, "HTTP/1.1 200 OK", 15) != 0) {
            elog(ERROR, "Error in response from Docker API socket: '%s'", buf);
            return -1;
        }

        if (docker_parse_port_mapping(buf, port) == 0) {
            break;
        }

        if (strstr(buf, "\r\n0\r\n")) {
            elog(ERROR, "Error - cannot find port mapping information for container");
            return -1;
        }
    }

    return 0;
}

static int docker_call(int sockfd, char *request, char **response, int silent) {
    int res = 0;

    if (!silent) {
        elog(DEBUG1, "Docker API request:\n%s", request);
    }

    res = send_message(sockfd, request);
    if (res < 0) {
        elog(ERROR, "Error sending data to the Docker API socket during container create call");
        return -1;
    }

    *response = palloc(8192);
    res = recv_message(sockfd, *response, 8192);
    if (res < 0) {
        elog(ERROR, "Error receiving data from the Docker API socket during container create call");
        return -1;
    }

    /* Print Docker response */
    if (!silent) {
        elog(DEBUG1, "Docker API response:\n%s", *response);
    }

    return 0;
}

static int plc_docker_container_command(int sockfd, char *name, const char *cmd, int silent) {
    char  message[2048];
    char  apiendpoint[200];
    char *response = NULL;
    int   res = 0;

    /* Get Docker API endpoint for current API version */
    sprintf(apiendpoint,
            "/%s/containers/%s/%s",       // URL to request
            plc_docker_api_version,       // Docker API version we use
            name,                         // Container name
            cmd);                         // Command to execute

    /* Fill in the HTTP message */
    sprintf(message,
            plc_docker_post_message_text, // POST message template
            apiendpoint,                  // API endpoint to call
            0,                            // Content-Length of the message we passing
            "");                          // Content of the message (empty message)

    docker_call(sockfd, message, &response, silent);

    if (response) {
        pfree(response);
    }

    return res;
}

int plc_docker_connect() {
    struct sockaddr_un  address;
    int                 sockfd;

    /* Create the socket */
    sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        elog(ERROR, "Error creating UNIX socket for Docker API");
        return -1;
    }

    /* Clean up address structure and fill the socket address */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, plc_docker_socket);

    /* connect the socket */
    if (connect(sockfd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        elog(ERROR, "Error connecting to the Docker API socket '%s'", plc_docker_socket);
        return -1;
    }

    return sockfd;
}

int plc_docker_create_container(int sockfd, plcContainer *cont, char **name) {
    char  message[2048];
    char  message_body[2048];
    char  apiendpoint[200];
    char *response = NULL;
    char *sharing;
    int   res = 0;

    /* Get Docker API endpoint for current API version */
    sprintf(apiendpoint,
            "/%s/containers/create",
            plc_docker_api_version);

    /* Get Docket API "create" call JSON message body */
    sharing = get_sharing_options(cont);
    sprintf(message_body,
            plc_docker_create_request,
            cont->command,
            cont->dockerid,
            sharing,
            ((long long)cont->memoryMb) * 1024 * 1024);
    pfree(sharing);

    /* Fill in the HTTP message */
    sprintf(message,
            plc_docker_post_message_json, // HTTP POST request template
            apiendpoint,                  // API endpoint to call
            strlen(message_body),         // Content-length
            message_body);                // POST message JSON content

    docker_call(sockfd, message, &response, 0);

    res = docker_parse_container_id(response, name);
    if (res < 0) {
        elog(ERROR, "Error parsing container ID");
        return -1;
    }

    if (response) {
        pfree(response);
    }

    return res;
}

int plc_docker_start_container(int sockfd, char *name) {
    return plc_docker_container_command(sockfd, name, "start", 0);
}

int plc_docker_inspect_container(int sockfd, char *name, int *port) {
    char message[200];
    char response[8192];
    int  res = 0;

    /* Fill in the HTTP message */
    sprintf(message,
            plc_docker_get_message,
            plc_docker_api_version,
            name);
    elog(DEBUG1, "Docker API request:\n%s", message);

    res = send_message(sockfd, message);
    if (res < 0) {
        elog(ERROR, "Error sending data to the Docker API socket during container delete call");
        return -1;
    }

    res = recv_port_mapping(sockfd, response, 8192, port);
    if (res < 0) {
        elog(ERROR, "Error receiving data from the Docker API socket during container delete call");
        return -1;
    }

    /* Print Docker response */
    elog(DEBUG1, "Docker API response:\n%s", response);

    return res;
}

int plc_docker_wait_container(int sockfd, char *name) {
    return plc_docker_container_command(sockfd, name, "wait", 1);
}

int plc_docker_delete_container(int sockfd, char *name) {
    char  message[2048];
    char *response = NULL;
    int   res = 0;

    /* Fill in the HTTP message */
    sprintf(message,
            plc_docker_delete_request, // Delete message template
            plc_docker_api_version,    // API version
            name);                     // Container name

    docker_call(sockfd, message, &response, 1);

    if (response) {
        pfree(response);
    }

    return res;
}

int plc_docker_disconnect(int sockfd) {
    return close(sockfd);
}