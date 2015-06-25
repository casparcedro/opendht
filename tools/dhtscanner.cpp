/*
 *  Copyright (C) 2014 Savoir-Faire Linux Inc.
 *
 *  Author: Adrien Béraud <adrien.beraud@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *  Additional permission under GNU GPL version 3 section 7:
 *
 *  If you modify this program, or any covered work, by linking or
 *  combining it with the OpenSSL project's OpenSSL library (or a
 *  modified version of that library), containing parts covered by the
 *  terms of the OpenSSL or SSLeay licenses, Savoir-Faire Linux Inc.
 *  grants you additional permission to convey the resulting work.
 *  Corresponding Source for a non-source form of such a combination
 *  shall include the source code for the parts of OpenSSL used as well
 *  as that of the covered work.
 */

#include <opendht.h>

extern "C" {
#include <gnutls/gnutls.h>
}

#include <iostream>
#include <string>
#include <chrono>
#include <set>
#include <condition_variable>
#include <mutex>

using namespace dht;

struct snode_compare {
    bool operator() (const std::shared_ptr<Dht::Node>& lhs, const std::shared_ptr<Dht::Node>& rhs) const{
        return lhs->id < rhs->id;
    }
};

using NodeSet = std::set<std::shared_ptr<Dht::Node>, snode_compare>;
std::condition_variable cv;

void
step(DhtRunner& dht, std::atomic_uint& done, std::shared_ptr<NodeSet> all_nodes, dht::InfoHash cur_h, unsigned cur_depth)
{
    std::cout << "step at " << cur_h << ", depth " << cur_depth << std::endl;
    done++;
    dht.get(cur_h, [all_nodes](const std::vector<std::shared_ptr<Value>>& values) {
        return true;
    }, [&,all_nodes,cur_h,cur_depth](bool, const std::vector<std::shared_ptr<Dht::Node>>& nodes) {
        all_nodes->insert(nodes.begin(), nodes.end());
        NodeSet sbuck {nodes.begin(), nodes.end()};
        if (not sbuck.empty()) {
            unsigned sbuck_depth = InfoHash::commonBits((*sbuck.begin())->id, (*std::prev(sbuck.end()))->id)+3;
            std::cout << cur_h << " : " << nodes.size() << " nodes; bucket is " << sbuck_depth << " bits deep (cur " << cur_depth << ")" << std::endl;
            for (unsigned b = cur_depth ; b < sbuck_depth; b++) {
                auto new_h = cur_h;
                new_h.setBit(b, 1);
                step(dht, done, all_nodes, new_h, b+1);
            }
        }
        done--;
        std::cout << done.load() << " operations left, " << all_nodes->size() << " nodes found." << std::endl;
        cv.notify_one();
    });
}

int
main(int argc, char **argv)
{
    int i = 1;
    in_port_t port = 0;
    if (argc >= 2) {
        int p = atoi(argv[i]);
        if (p > 0 && p < 0x10000) {
            port = p;
            i++;
        }
    }
    if (!port)
        port = 4222;

    int rc = gnutls_global_init();
    if (rc != GNUTLS_E_SUCCESS)
        throw std::runtime_error(std::string("Error initializing GnuTLS: ")+gnutls_strerror(rc));

    auto ca_tmp = dht::crypto::generateIdentity("DHT Node CA");
    auto crt_tmp = dht::crypto::generateIdentity("Scanner node", ca_tmp);

    DhtRunner dht;
    dht.run(port, crt_tmp, true, [](dht::Dht::Status /* ipv4 */, dht::Dht::Status /* ipv6 */) {});

    while (i+1 < argc) {
        dht.bootstrap(argv[i], argv[i + 1]);
        i += 2;
    }

    std::cout << "OpenDht node " << dht.getNodeId() << " running on port " <<  port<<  std::endl;
    std::cout << "Scanning network..." << std::endl;
    auto all_nodes = std::make_shared<NodeSet>();

    dht::InfoHash cur_h {};
    cur_h.setBit(8*HASH_LEN-1, 1);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::atomic_uint done {false};
    step(dht, done, all_nodes, cur_h, 0);

    {
        std::mutex m;
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&](){
            return done.load() == 0;
        });
    }

    std::cout << std::endl << "Scan ended: " << all_nodes->size() << " nodes found." << std::endl;
    for (const auto& n : *all_nodes)
        std::cout << "Node " << *n << std::endl;

    dht.join();
    gnutls_global_deinit();
    return 0;
}
