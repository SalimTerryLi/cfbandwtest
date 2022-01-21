#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <memory.h>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include <curl/curl.h>

#define log(format, ...) fprintf (stderr, format, ##__VA_ARGS__)
#define output(format, ...) fprintf (stdout, format, ##__VA_ARGS__)

// run-time configuration
static uint16_t parallel_count = 1;
static uint16_t max_test_period_s = 15;
static uint16_t max_in_band_mBs = 0;
static char *remote_file_url_p = NULL;

int test_band();

void print_help(const char *execname) {
    log("%s [options] file-url\n", execname);
    log("Designed to fetch a large file from the provided URL, with underlying TCP connection to different IPs\n");
    log("So that multiple CDN IPs can be tested conveniently to find out which works better\n");
    log("IPs should be fed through stdin, one per line\n");
    log("EOF on pipe will stop the program from waiting for new IPs\n");
    log("Test results will be printed to stdout\n");
    log("Options:\n");
    log("  -h       show this page\n");
    log("  -p n     max parallel task count, default 1\n");
    log("  -t n     max testing period in second for each connection, default 15\n");
    log("  -b n     max available incoming bandwidth in MB/s, must be provided if parallel count greater than 1\n\n");
}

char *readline(char *recv_buf, size_t size, int fd) {
    size_t buf_idx = 0;
    while (buf_idx < size) {
        long rd_len = read(0, recv_buf + buf_idx, 1);
        if (rd_len == 0) {
            errno = 0;
            return NULL;
        } else if (rd_len == -1) {
            if (errno == EAGAIN) {
                // no data available in buffer
                return NULL;
            } else {
                printf("failed to read from fd: %u %s", errno, strerror(errno));
                return NULL;
            }
        } else {
            if (recv_buf[buf_idx] == '\n' || recv_buf[buf_idx] == '\0') {
                recv_buf[buf_idx] = '\0';
                return recv_buf;
            } else {
                ++buf_idx;
            }
        }
    }
    log("buf too small\n");
    return NULL;
}

int main(int argc, char** argv) {
    int c;
    char *endptr;
    while ((c = getopt (argc, argv, "hp:t:b:")) != -1) {
        switch (c) {
            case 'h':
                print_help(*argv);
                exit(0);

            case 'p':
                errno = 0;
                parallel_count = (uint16_t)strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || *endptr != '\0') {
                    log("failed to parse argument of -p: %s\n", optarg);
                    exit(-1);
                }
                if (parallel_count == 0) {
                    log("set parallel to 1...\n");
                    parallel_count = 1;
                }
                break;

            case 't':
                errno = 0;
                max_test_period_s = (uint16_t)strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || *endptr != '\0') {
                    log("failed to parse argument of -t: %s\n", optarg);
                    exit(-1);
                }
                break;

            case 'b':
                errno = 0;
                max_in_band_mBs = (uint16_t)strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || *endptr != '\0') {
                    log("failed to parse argument of -b: %s\n", optarg);
                    exit(-1);
                }
                break;

            case '?':
                log("unrecognized option: -%c\nrun with -h to see help page\n", optopt);
                exit(-1);

            default:
                abort();
        }
    }

    if (optind < argc) {
        remote_file_url_p = argv[optind];
    } else {
        log("no url provided!\n");
        return -1;
    }

    if (parallel_count > 1 && max_in_band_mBs == 0) {
        log("`-p` must used with `-b`!\n");
        return -1;
    }

    log("start testing with %u parallel tasks, bandwidth hint %uMB/s, max period %us, file url: %s\n", parallel_count, max_in_band_mBs, max_test_period_s, remote_file_url_p);
    // make stdin non-blocking
    int fd_stat = fcntl(0, F_GETFL);
    assert(fcntl(0, F_SETFL, fd_stat | O_NONBLOCK) == 0);
    int ret = test_band();
    // reset stdin
    fcntl(0, F_SETFL, fd_stat);
    return ret;
}

int test_band() {
    CURLMcode curlm_code;
    CURLcode curle_code;

    // setup libcurl and multi interface
    curle_code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curle_code != CURLE_OK) {
        log("curl_global_init() failed: %s\n", curl_easy_strerror(curle_code));
        goto err_global_init;
    }
    CURLM *curl_hdl = curl_multi_init();
    if (curl_hdl == NULL) {
        log("curl_multi_init() failed\n");
        goto err_multi_init;
    }

    typedef struct {
        CURL *req_hdl;
        struct curl_slist *connect_to;
        bool is_running;
    } task_handler_t;

    task_handler_t *hdls = malloc(sizeof(task_handler_t) * parallel_count);
    memset(hdls, 0, sizeof(task_handler_t) * parallel_count);
    if (hdls == NULL) {
        log("failed to malloc task_handlers: %s\n", strerror(errno));
        goto err_fail_alloc_task_handlers;
    }
    for (task_handler_t *hdl = hdls; hdl < hdls + parallel_count; ++hdl) {
        hdl->req_hdl = curl_easy_init();
        if (hdl->req_hdl == NULL) {
            log("curl_easy_init() failed\n");
            goto err_curl_easy_init;
        }
        // configure opts in advance
        assert(curl_easy_setopt(hdl->req_hdl, CURLOPT_URL, remote_file_url_p) == CURLE_OK);
        hdl->is_running = false;
    }

    fd_set curl_rd_fdset;
    fd_set curl_wr_fdset;
    fd_set curl_er_fdset;
    int curl_max_fd;

    bool should_exit = false;               // should be set in curl event, whether success or failure
    bool is_stdin_closed = false;           // mark the current state of stdin
    size_t spare_hdl_count = parallel_count;     // whether next task should be loaded or not
    int still_running;  // dummy
    while (!should_exit) {
        // prepare fdset
        FD_ZERO(&curl_rd_fdset);
        FD_ZERO(&curl_wr_fdset);
        FD_ZERO(&curl_er_fdset);
        assert(curl_multi_fdset(curl_hdl, &curl_rd_fdset, &curl_wr_fdset, &curl_er_fdset, &curl_max_fd) == CURLE_OK);
        if ((!is_stdin_closed) && spare_hdl_count != 0) {
            FD_SET(0, &curl_rd_fdset);
            FD_SET(0, &curl_er_fdset);
        }

        // do select
        if (curl_max_fd == -1) {
            curl_max_fd = 1;
        }
        struct timeval timeout;
        struct timeval *timeout_p = NULL;
        long timeo;
        assert(curl_multi_timeout(curl_hdl, &timeo) == CURLM_OK);
        if(timeo < 0) {
            //
        } else {
            timeout.tv_sec = timeo / 1000;
            timeout.tv_usec = (timeo % 1000) * 1000;
            timeout_p = &timeout;
        }
        int sel_ret = select(curl_max_fd +1, &curl_rd_fdset, &curl_wr_fdset, &curl_er_fdset, timeout_p);
        if (sel_ret == -1) {
            log("select() error: %s\n", strerror(errno));
        } else {
            // set flags but deal with stdin event at last
            bool update_stdin_input = false;
            if (FD_ISSET(0, &curl_rd_fdset)) {
                update_stdin_input = true;
                --sel_ret;
            }
            if (FD_ISSET(0, &curl_er_fdset)) {
                log("unexpected Exception fdset on stdin\n");
                --sel_ret;
            }

            if (sel_ret > 0) {
                // socket event still exists
                curlm_code = curl_multi_perform(curl_hdl, &still_running);
                assert(curlm_code == CURLM_OK);
                int msgs_left = -1;
                while (true) {
                    CURLMsg *msg = curl_multi_info_read(curl_hdl, &msgs_left);
                    if (msg == NULL) {
                        break;
                    }
                    task_handler_t *curr_hdl = NULL;
                    // find out the finished task
                    for (task_handler_t *hdl = hdls; hdl < hdls + parallel_count; ++hdl) {
                        if (hdl->req_hdl == msg->easy_handle) {
                            curr_hdl = hdl;
                            break;
                        }
                    }
                    assert(curr_hdl != NULL);
                    if (msg->msg == CURLMSG_DONE) {
                        if (msg->data.result != CURLE_OK) {
                            log("!--error task idx=%ld: %s\n", curr_hdl - hdls, curl_easy_strerror(msg->data.result));
                        }
                    } else {
                        log("E: CURLMsg (%d)\n", msg->msg);
                    }
                    // cleanup finished task
                    curl_slist_free_all(curr_hdl->connect_to);
                    curr_hdl->connect_to = NULL;
                    curr_hdl->is_running = false;
                    curl_multi_remove_handle(curl_hdl, curr_hdl->req_hdl);
                    ++spare_hdl_count;
                    log("<--task %ld exited. remain %lu working tasks\n", curr_hdl - hdls, parallel_count - spare_hdl_count);
                }
            } else {
                // deal with curl provided timout. May over-call but that doesn't matter
                assert(curl_multi_perform(curl_hdl, &still_running) == CURLM_OK);
            }

            // then deal with stdin input
            if (update_stdin_input && spare_hdl_count != 0) {
                char buf[64];
                char *line = readline(buf, sizeof(buf), 0);
                if (line == NULL) {
                    if (errno == EWOULDBLOCK) {
                        // buffer empty
                    } else {
                        log("EOF received. exiting\n");
                        is_stdin_closed = true;
                    }
                } else {
                    log("-->new task: fetch %s with connection to %s\n", remote_file_url_p, buf);
                    task_handler_t *next_hdl;
                    spare_hdl_count = 0;
                    for (task_handler_t *hdl = hdls; hdl < hdls + parallel_count; ++hdl) {
                        if (!hdl->is_running) {
                            ++spare_hdl_count;
                            next_hdl = hdl;
                        }
                    }
                    --spare_hdl_count;
                    char ip_format[128];
                    snprintf(ip_format, sizeof(ip_format), "::%s:", buf);
                    next_hdl->connect_to = curl_slist_append(NULL, ip_format);
                    assert(curl_easy_setopt(next_hdl->req_hdl, CURLOPT_CONNECT_TO, (char*)next_hdl->connect_to) == CURLE_OK);
                    assert(curl_multi_add_handle(curl_hdl, next_hdl->req_hdl) == CURLM_OK);
                    assert(curl_multi_perform(curl_hdl, &still_running) == CURLM_OK);
                    next_hdl->is_running = true;
                    log("-->new task started at idx=%ld, remain %zu slots\n", next_hdl - hdls, spare_hdl_count);
                }
            }

            if (is_stdin_closed && spare_hdl_count == parallel_count) {
                should_exit = true;
            }
        }
    }
    log("exited\n");

err_curl_easy_init:
    for (task_handler_t *hdl = hdls; hdl < hdls + parallel_count; ++hdl) {
        if (hdl->req_hdl != NULL) {
            curl_easy_cleanup(hdl->req_hdl);
        }
        if (hdl->connect_to != NULL) {
            curl_slist_free_all(hdl->req_hdl);
        }
    }
    free(hdls);
err_fail_alloc_task_handlers:
    curl_multi_cleanup(curl_hdl);
err_multi_init:
    curl_global_cleanup();
err_global_init:
    return 0;
}