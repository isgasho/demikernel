// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "common.hh"
#include <arpa/inet.h>
#include <boost/optional.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <cassert>
#include <cstring>
#include <dmtr/annot.h>
#include <boost/chrono.hpp>
#include <dmtr/latency.h>
#include <fcntl.h>
#include <iostream>
#include <dmtr/libos/mem.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <yaml-cpp/yaml.h>
#include <sys/stat.h>
#include <sys/types.h>

int lqd = 0;
dmtr_latency_t *pop_latency = NULL;
dmtr_latency_t *push_latency = NULL;
dmtr_latency_t *e2e_latency = NULL;
dmtr_latency_t *file_log_latency = NULL;

namespace po = boost::program_options;
int lfd = 0, epoll_fd, ffd;

void sig_handler(int signo)
{
    dmtr_dump_latency(stderr, e2e_latency);
    dmtr_dump_latency(stderr, pop_latency);
    dmtr_dump_latency(stderr, push_latency);
    dmtr_dump_latency(stderr, file_log_latency);
    close(lfd);
    close(epoll_fd);
    exit(0);
}


int process_read(int fd, char *buf)
{
    int ret;
    auto t0 = boost::chrono::steady_clock::now();
    ret = read(fd,
               (void *)&(buf[0]),
               packet_size);
    auto dt = boost::chrono::steady_clock::now() - t0;
    DMTR_OK(dmtr_record_latency(pop_latency, dt.count()));
    return ret;
}


int main(int argc, char *argv[])
{
    parse_args(argc, argv, true);

    std::cerr << "packet_size is: " << packet_size << std::endl;

    struct sockaddr_in saddr = {};
    saddr.sin_family = AF_INET;
    if (boost::none == server_ip_addr) {
        std::cerr << "Listening on `*:" << port << "`..." << std::endl;
        saddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        const char *s = boost::get(server_ip_addr).c_str();
        std::cerr << "Listening on `" << s << ":" << port << "`..." << std::endl;
        if (inet_pton(AF_INET, s, &saddr.sin_addr) != 1) {
            std::cerr << "Unable to parse IP address." << std::endl;
            return -1;
        }
    }
    saddr.sin_port = htons(port);

    DMTR_OK(dmtr_new_latency(&pop_latency, "server pop"));
    DMTR_OK(dmtr_new_latency(&push_latency, "server push"));
    DMTR_OK(dmtr_new_latency(&e2e_latency, "server end-to-end"));
    DMTR_OK(dmtr_new_latency(&file_log_latency, "file log server"));


    lfd = socket(AF_INET, SOCK_STREAM, 0);
    std::cout << "listen qd: " << lfd << std::endl;

    // Put it in non-blocking mode
    DMTR_OK(fcntl(lfd, F_SETFL, O_NONBLOCK, 1));

    // Set TCP_NODELAY
    int n = 1;
    if (setsockopt(lfd, IPPROTO_TCP,
                   TCP_NODELAY, (char *)&n, sizeof(n)) < 0) {
        exit(-1);
    }

    DMTR_OK(bind(lfd, reinterpret_cast<struct sockaddr *>(&saddr), sizeof(saddr)));

    std::cout << "listening for connections\n";
    listen(lfd, 3);

    if (signal(SIGINT, sig_handler) == SIG_ERR)
        std::cout << "\ncan't catch SIGINT\n";
    if (signal(SIGPIPE, sig_handler) == SIG_ERR)
        std::cout << "\ncan't catch SIGPIPE\n";

    epoll_fd = epoll_create1(0);
    struct epoll_event event, events[10];
    event.events = EPOLLIN;
    event.data.fd = lfd;
    DMTR_OK(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, lfd, &event));
    if (boost::none != file) {
        // open a log file
        ffd = open(boost::get(file).c_str(), O_RDWR | O_CREAT | O_SYNC, S_IRWXU | S_IRGRP);
    }

    while (1) {
        int event_count = epoll_wait(epoll_fd, events, 10, -1);

        for (int i = 0; i < event_count; i++) {
            //std::cout << "Found something!" << std::endl;
            if (events[i].data.fd == lfd) {
                // run accept
                std::cout << "Found new connection" << std::endl;
                int newfd = accept(lfd, NULL, NULL);

                // Put it in non-blocking mode
                // COMMENTED OUT FOR NOW TO SEE IF FIX BUG
                DMTR_OK(fcntl(newfd, F_SETFL, O_NONBLOCK, 1));

                // Set TCP_NODELAY
                int n = 1;
                if (setsockopt(newfd, IPPROTO_TCP,
                               TCP_NODELAY, (char *)&n, sizeof(n)) < 0) {
                    exit(-1);
                }

                event.events = EPOLLIN | EPOLLET;
                event.data.fd = newfd;
                DMTR_OK(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, newfd, &event));
            } else {
                char *buf = (char *)malloc(packet_size);
                int read_ret = process_read(events[i].data.fd, buf);
                if (read_ret < 0) {
                    free(buf);
                    DMTR_OK(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, &events[i]));
                    close(events[i].data.fd);
                    continue;
                } else if (read_ret == 0) continue;
                if (0 != ffd) {
                    // log to file
                    auto t0 = boost::chrono::steady_clock::now();
                    ssize_t total=0, written = 0;
                    do {
                        written = write(ffd, &buf[total], read_ret - total);
                        if (written < 0) return written;
                        total += written;
                    } while (total < read_ret);
                    //int ret = fsync(ffd);
                    //assert(ret == 0);
                    auto log_dt = boost::chrono::steady_clock::now() - t0;
                    DMTR_OK(dmtr_record_latency(file_log_latency, log_dt.count()));
                }
                auto t0 = boost::chrono::steady_clock::now();
                ssize_t total=0, write_ret = 0;
                do {
                    write_ret = write(events[i].data.fd,
                                      (void *)&(buf[total]),
                                      read_ret - total);
                    if (write_ret < 0) {
                        free(buf);
                        DMTR_OK(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, &events[i]));
                        close(events[i].data.fd);
                        continue;
                    }
                    total += write_ret;
                } while (total < read_ret);
                auto dt = boost::chrono::steady_clock::now() - t0;
                DMTR_OK(dmtr_record_latency(push_latency, dt.count()));
                DMTR_OK(dmtr_record_latency(e2e_latency, dt.count()));

                free(buf);
            }
        }
    }
}


