// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

#include <ctype.h>
#include "cli.h"
#include "cluster_client.h"
#include "str_util.h"

#include <algorithm>

// Safely allocate an OSD number
struct alloc_osd_t
{
    cli_tool_t *parent;

    uint64_t new_id = 1;

    int state = 0;
    cli_result_t result;

    bool is_done()
    {
        return state == 100;
    }

    void loop()
    {
        if (state == 1)
            goto resume_1;
        do
        {
            parent->etcd_txn(json11::Json::object {
                { "compare", json11::Json::array {
                    json11::Json::object {
                        { "target", "VERSION" },
                        { "version", 0 },
                        { "key", base64_encode(
                            parent->cli->st_cli.etcd_prefix+"/osd/stats/"+std::to_string(new_id)
                        ) },
                    },
                } },
                { "success", json11::Json::array {
                    json11::Json::object {
                        { "request_put", json11::Json::object {
                            { "key", base64_encode(
                                parent->cli->st_cli.etcd_prefix+"/osd/stats/"+std::to_string(new_id)
                            ) },
                            { "value", base64_encode("{}") },
                        } },
                    },
                } },
                { "failure", json11::Json::array {
                    json11::Json::object {
                        { "request_range", json11::Json::object {
                            { "key", base64_encode(parent->cli->st_cli.etcd_prefix+"/osd/stats/") },
                            { "range_end", base64_encode(parent->cli->st_cli.etcd_prefix+"/osd/stats0") },
                            { "keys_only", true },
                        } },
                    },
                } },
            });
        resume_1:
            state = 1;
            if (parent->waiting > 0)
                return;
            if (parent->etcd_err.err)
            {
                result = parent->etcd_err;
                state = 100;
                return;
            }
            if (!parent->etcd_result["succeeded"].bool_value())
            {
                std::vector<osd_num_t> used;
                parent->iterate_kvs_1(parent->etcd_result["responses"][0]["response_range"]["kvs"], "/osd/stats/", [&](uint64_t cur_osd, json11::Json value)
                {
                    used.push_back(cur_osd);
                });
                std::sort(used.begin(), used.end());
                if (used[used.size()-1] == used.size())
                {
                    new_id = used.size()+1;
                }
                else
                {
                    int s = 0, e = used.size();
                    while (e > s+1)
                    {
                        int c = (s+e)/2;
                        if (used[c] == c+1)
                            s = c;
                        else
                            e = c;
                    }
                    new_id = used[e-1]+1;
                }
            }
        } while (!parent->etcd_result["succeeded"].bool_value());
        state = 100;
        result = (cli_result_t){
            .text = std::to_string(new_id),
            .data = json11::Json(new_id),
        };
    }
};

std::function<bool(cli_result_t &)> cli_tool_t::start_alloc_osd(json11::Json cfg)
{
    auto alloc_osd = new alloc_osd_t();
    alloc_osd->parent = this;
    return [alloc_osd](cli_result_t & result)
    {
        alloc_osd->loop();
        if (alloc_osd->is_done())
        {
            result = alloc_osd->result;
            delete alloc_osd;
            return true;
        }
        return false;
    };
}
