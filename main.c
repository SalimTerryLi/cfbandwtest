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
#include <time.h>

#include <curl/curl.h>

#define log(format, ...) fprintf (stderr, format, ##__VA_ARGS__)
#define output(format, ...) fprintf (stdout, format, ##__VA_ARGS__)

// compile-time configuration
#define SPEED_CHECK_PERIOD_MS 1000
#define SPEED_CHECK_ACCEPT_COUNT 5
#define SPEED_CHECK_MIN_DIFF 0.1l

// run-time configuration
static uint16_t parallel_count = 1;
static uint16_t max_test_period_s = 15;
static uint16_t max_in_band_mBs = 0;
static char *remote_file_url_p = NULL;

static int test_band();

static void print_help(const char *execname) {
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

static char *readline(char *recv_buf, size_t size, int fd) {
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

static uint64_t get_timestamp() {
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);
    return spec.tv_nsec / 1000000 + spec.tv_sec * 1000;
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

typedef struct {
    CURL *req_hdl;
    struct curl_slist *connect_to;
    bool is_running;
    // below is used to calculate band
    uint64_t begin_timestamp;           // will only be filled on startup
    uint16_t accept_count;              // accepted count of available bandwidth
    uint32_t max_bandw_kbs;             // recorded maximum bandwidth
    uint32_t speed_up_period_ms;        // time period of bandwidth increasing
    uint32_t avg_bandwidth_kbs;         // average bandwidth
    // below is used to provide estimated transfer speed
    uint64_t last_checked_timestamp;
    uint64_t last_transfered_size_B;
    uint64_t total_transfered_size_B;
} task_handler_t;

static task_handler_t *hdls = NULL;

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    task_handler_t *hdl = (task_handler_t*) userdata;
    hdl->last_transfered_size_B += nmemb;
    return nmemb;
}

static bool update_bandwidth_calc(task_handler_t *task, uint64_t curr_timestamp) {
    task->total_transfered_size_B += task->last_transfered_size_B;
    uint32_t measured_bandwidth_kbs_curr = task->last_transfered_size_B / 1024 * 1000 / SPEED_CHECK_PERIOD_MS;
    log("%s curr band: %ukb/s, max=%ukb/s\n",
        task->connect_to->data,
        measured_bandwidth_kbs_curr,
        task->max_bandw_kbs
        );
    if (measured_bandwidth_kbs_curr > task->max_bandw_kbs) {
        if (measured_bandwidth_kbs_curr > task->max_bandw_kbs * (1 + SPEED_CHECK_MIN_DIFF)) {
            // only reset counter if incremental is great enough
            task->accept_count = 0;
            task->avg_bandwidth_kbs = 0;
        }
        task->max_bandw_kbs = measured_bandwidth_kbs_curr;
    } else {
        ++task->accept_count;
        if (task->accept_count == 1) {
            task->speed_up_period_ms = curr_timestamp - task->begin_timestamp;
        } else if (task->accept_count == SPEED_CHECK_ACCEPT_COUNT) {
            // end of measurement.
            return true;
        }
        // update average bandwidth
        task->avg_bandwidth_kbs = (uint32_t)((double)task->avg_bandwidth_kbs * (task->accept_count - 1) / task->accept_count
                                             + (double)measured_bandwidth_kbs_curr * 1 / task->accept_count);
    }

    if (curr_timestamp - task->begin_timestamp >= max_test_period_s * 1000) {
        if (task->avg_bandwidth_kbs == 0) {
            task->avg_bandwidth_kbs = (uint32_t)(task->max_bandw_kbs * (1 - SPEED_CHECK_MIN_DIFF));
        }
        return true;
    }

    return false;
}

static int test_band() {
    output("%s\t\t%s\t%s\t%s\t%s\t%s\n",
           "TARGET",
           "AVG",
           "MAX",
           "SPEEDUP",
           "TOTAL",
           "ERRRFLAG"
    );

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

    hdls = malloc(sizeof(task_handler_t) * parallel_count);
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
        // configure callback in advance
        assert(curl_easy_setopt(hdl->req_hdl, CURLOPT_WRITEFUNCTION, write_callback) == CURLE_OK);
        // configure corresponding userdata
        assert(curl_easy_setopt(hdl->req_hdl, CURLOPT_WRITEDATA, (void*)hdl) == CURLE_OK);
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
        // always assume stdin is available for polling
        if (curl_max_fd == -1) {
            curl_max_fd = 1;
        }

        // setup timeout
        struct timeval timeout;
        struct timeval *timeout_p = NULL;       // default no timeout
        long timeout_curl;
        long timeout_tasks = LONG_MAX;
        assert(curl_multi_timeout(curl_hdl, &timeout_curl) == CURLM_OK);
        {
            // hide vars
            uint64_t curr_ts = get_timestamp();
            // check and update all tasks' timeout
            for (task_handler_t *hdl = hdls; hdl < hdls + parallel_count; ++hdl) {
                if (hdl->is_running) {
                    long this_task_elapsed = (long)(curr_ts - hdl->last_checked_timestamp);
                    long this_task_timeout = this_task_elapsed >= SPEED_CHECK_PERIOD_MS? 0: SPEED_CHECK_PERIOD_MS - this_task_elapsed;
                    // also do bandwidth calc here
                    if (this_task_elapsed >= SPEED_CHECK_PERIOD_MS) {
                        if (update_bandwidth_calc(hdl, curr_ts)) {
                            hdl->connect_to->data[strlen(hdl->connect_to->data)-1] = '\0';
                            log("<--test complete: target=%s\n", hdl->connect_to->data + 2);
                            output("%s\t%u\t%u\t%u\t%lu\n",
                                   hdl->connect_to->data + 2,
                                   hdl->avg_bandwidth_kbs,
                                   hdl->max_bandw_kbs,
                                   hdl->speed_up_period_ms,
                                   hdl->total_transfered_size_B / 1024
                                   );
                            // cleanup finished task
                            curl_slist_free_all(hdl->connect_to);
                            hdl->connect_to = NULL;
                            hdl->is_running = false;
                            curl_multi_remove_handle(curl_hdl, hdl->req_hdl);
                            ++spare_hdl_count;
                        } else {
                            hdl->last_checked_timestamp = curr_ts;
                            hdl->last_transfered_size_B = 0;
                        }
                    }
                    // find out the shortest timeout
                    if (this_task_timeout < timeout_tasks) {
                        timeout_tasks = this_task_timeout;
                    }
                }
            }
            // so that if any task is running, timeout_tasks will be filled with a timeout that is nearest to the next checkpoint
        }
        if (timeout_curl < 0 && timeout_tasks == LONG_MAX) {
            // no timeout is expected. do nothing and select forever
        } else if (timeout_curl >= 0 && timeout_tasks == LONG_MAX) {
            // only curl is requesting timeout
            timeout.tv_sec = timeout_curl / 1000;
            timeout.tv_usec = (timeout_curl % 1000) * 1000;
            timeout_p = &timeout;
        } else if (timeout_curl < 0 && timeout_tasks != LONG_MAX) {
            // only tasks is requesting timeout
            timeout.tv_sec = timeout_tasks / 1000;
            timeout.tv_usec = (timeout_tasks % 1000) * 1000;
            timeout_p = &timeout;
        } else {
            // both are requesting a value
            timeout_curl = timeout_curl > timeout_tasks? timeout_curl: timeout_tasks;
            timeout.tv_sec = timeout_curl / 1000;
            timeout.tv_usec = (timeout_curl % 1000) * 1000;
            timeout_p = &timeout;
        }

        // select!
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
                            output("%s\t%u\t%u\t%u\t%lu\tERROR\n",
                                   curr_hdl->connect_to->data + 2,
                                   curr_hdl->avg_bandwidth_kbs,
                                   curr_hdl->max_bandw_kbs,
                                   curr_hdl->speed_up_period_ms,
                                   curr_hdl->total_transfered_size_B / 1024
                            );
                        } else {
                            output("%s\t%u\t%u\t%u\t%lu\tEARLY_CLOSE\n",
                                   curr_hdl->connect_to->data + 2,
                                   curr_hdl->avg_bandwidth_kbs,
                                   curr_hdl->max_bandw_kbs,
                                   curr_hdl->speed_up_period_ms,
                                   curr_hdl->total_transfered_size_B / 1024
                            );
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
                }
            } else if (sel_ret == 0 && !update_stdin_input) {
                // deal with curl provided timout.
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
                    next_hdl->accept_count = 0;
                    next_hdl->avg_bandwidth_kbs = 0;
                    next_hdl->max_bandw_kbs = 0;
                    next_hdl->speed_up_period_ms = 0;
                    next_hdl->last_checked_timestamp = get_timestamp();
                    next_hdl->begin_timestamp = next_hdl->last_checked_timestamp;
                    next_hdl->last_transfered_size_B = 0;
                    next_hdl->total_transfered_size_B = 0;
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