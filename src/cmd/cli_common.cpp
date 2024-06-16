// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

#include <unistd.h>
#include "str_util.h"
#include "cluster_client.h"
#include "cli.h"

void cli_tool_t::change_parent(inode_t cur, inode_t new_parent, cli_result_t *result)
{
    auto cur_cfg_it = cli->st_cli.inode_config.find(cur);
    if (cur_cfg_it == cli->st_cli.inode_config.end())
    {
        char buf[128];
        snprintf(buf, 128, "Inode 0x%jx disappeared", cur);
        *result = (cli_result_t){ .err = EIO, .text = buf };
        return;
    }
    inode_config_t new_cfg = cur_cfg_it->second;
    std::string cur_name = new_cfg.name;
    std::string cur_cfg_key = base64_encode(cli->st_cli.etcd_prefix+
        "/config/inode/"+std::to_string(INODE_POOL(cur))+
        "/"+std::to_string(INODE_NO_POOL(cur)));
    new_cfg.parent_id = new_parent;
    json11::Json::object cur_cfg_json = cli->st_cli.serialize_inode_cfg(&new_cfg);
    waiting++;
    cli->st_cli.etcd_txn_slow(json11::Json::object {
        { "compare", json11::Json::array {
            json11::Json::object {
                { "target", "MOD" },
                { "key", cur_cfg_key },
                { "result", "LESS" },
                { "mod_revision", new_cfg.mod_revision+1 },
            },
        } },
        { "success", json11::Json::array {
            json11::Json::object {
                { "request_put", json11::Json::object {
                    { "key", cur_cfg_key },
                    { "value", base64_encode(json11::Json(cur_cfg_json).dump()) },
                } }
            },
        } },
    }, [this, result, new_parent, cur, cur_name](std::string err, json11::Json res)
    {
        if (err != "")
        {
            *result = (cli_result_t){ .err = EIO, .text = "Error changing parent of "+cur_name+": "+err };
        }
        else if (!res["succeeded"].bool_value())
        {
            *result = (cli_result_t){ .err = EAGAIN, .text = "Image "+cur_name+" was modified during change" };
        }
        else if (new_parent)
        {
            auto new_parent_it = cli->st_cli.inode_config.find(new_parent);
            std::string new_parent_name = new_parent_it != cli->st_cli.inode_config.end()
                ? new_parent_it->second.name : "<unknown>";
            *result = (cli_result_t){
                .text = "Parent of layer "+cur_name+" (inode "+std::to_string(INODE_NO_POOL(cur))+
                    " in pool "+std::to_string(INODE_POOL(cur))+") changed to "+new_parent_name+
                    " (inode "+std::to_string(INODE_NO_POOL(new_parent))+" in pool "+std::to_string(INODE_POOL(new_parent))+")",
            };
        }
        else
        {
            *result = (cli_result_t){
                .text = "Parent of layer "+cur_name+" (inode "+std::to_string(INODE_NO_POOL(cur))+
                    " in pool "+std::to_string(INODE_POOL(cur))+") detached",
            };
        }
        waiting--;
        ringloop->wakeup();
    });
}

void cli_tool_t::etcd_txn(json11::Json txn)
{
    waiting++;
    cli->st_cli.etcd_txn_slow(txn, [this](std::string err, json11::Json res)
    {
        waiting--;
        if (err != "")
            etcd_err = (cli_result_t){ .err = EIO, .text = "Error communicating with etcd: "+err };
        else
            etcd_err = (cli_result_t){ .err = 0 };
        etcd_result = res;
        ringloop->wakeup();
    });
}

inode_config_t* cli_tool_t::get_inode_cfg(const std::string & name)
{
    for (auto & ic: cli->st_cli.inode_config)
    {
        if (ic.second.name == name)
        {
            return &ic.second;
        }
    }
    return NULL;
}

void cli_tool_t::parse_config(json11::Json::object & cfg)
{
    for (auto kv_it = cfg.begin(); kv_it != cfg.end();)
    {
        // Translate all options with - to _
        if (kv_it->first.find("-") != std::string::npos)
        {
            cfg[str_replace(kv_it->first, "-", "_")] = kv_it->second;
            cfg.erase(kv_it++);
        }
        else
            kv_it++;
    }
    if (cfg.find("no_color") != cfg.end())
        color = !cfg["no_color"].bool_value();
    else if (cfg.find("color") != cfg.end())
        color = cfg["color"].bool_value();
    else
        color = isatty(1);
    json_output = cfg["json"].bool_value();
    iodepth = cfg["iodepth"].uint64_value();
    if (!iodepth)
        iodepth = 32;
    parallel_osds = cfg["parallel_osds"].uint64_value();
    if (!parallel_osds)
        parallel_osds = 4;
    log_level = cfg["log_level"].int64_value();
    progress = cfg["progress"].uint64_value() ? true : false;
    list_first = cfg["wait_list"].uint64_value() ? true : false;
}

struct cli_result_looper_t
{
    ring_consumer_t consumer;
    cli_result_t result;
    std::function<bool(cli_result_t &)> loop_cb;
    std::function<void(const cli_result_t &)> complete_cb;
};

void cli_tool_t::loop_and_wait(std::function<bool(cli_result_t &)> loop_cb, std::function<void(const cli_result_t &)> complete_cb)
{
    auto *looper = new cli_result_looper_t();
    looper->loop_cb = loop_cb;
    looper->complete_cb = complete_cb;
    looper->consumer.loop = [this, looper]()
    {
        bool done = looper->loop_cb(looper->result);
        if (done)
        {
            ringloop->unregister_consumer(&looper->consumer);
            looper->loop_cb = NULL;
            looper->complete_cb(looper->result);
            ringloop->submit();
            delete looper;
            return;
        }
        ringloop->submit();
    };
    cli->on_ready([this, looper]()
    {
        ringloop->register_consumer(&looper->consumer);
        ringloop->wakeup();
    });
}

void cli_tool_t::iterate_kvs_1(json11::Json kvs, const std::string & prefix, std::function<void(uint64_t, json11::Json)> cb)
{
    bool is_pool = prefix == "/pool/stats/";
    for (auto & kv_item: kvs.array_items())
    {
        auto kv = cli->st_cli.parse_etcd_kv(kv_item);
        uint64_t num = 0;
        char null_byte = 0;
        // OSD or pool number
        int scanned = sscanf(kv.key.substr(cli->st_cli.etcd_prefix.size() + prefix.size()).c_str(), "%ju%c", &num, &null_byte);
        if (scanned != 1 || !num || is_pool && num >= POOL_ID_MAX)
        {
            fprintf(stderr, "Invalid key in etcd: %s\n", kv.key.c_str());
            continue;
        }
        cb(num, kv.value);
    }
}

void cli_tool_t::iterate_kvs_2(json11::Json kvs, const std::string & prefix, std::function<void(pool_id_t pool_id, uint64_t num, json11::Json)> cb)
{
    bool is_inode = prefix == "/config/inode/" || prefix == "/inode/stats/";
    for (auto & kv_item: kvs.array_items())
    {
        auto kv = cli->st_cli.parse_etcd_kv(kv_item);
        pool_id_t pool_id = 0;
        uint64_t num = 0;
        char null_byte = 0;
        // pool+pg or pool+inode
        int scanned = sscanf(kv.key.substr(cli->st_cli.etcd_prefix.size() + prefix.size()).c_str(),
            "%u/%ju%c", &pool_id, &num, &null_byte);
        if (scanned != 2 || !pool_id || is_inode && INODE_POOL(num) || !is_inode && num >= UINT32_MAX)
        {
            fprintf(stderr, "Invalid key in etcd: %s\n", kv.key.c_str());
            continue;
        }
        cb(pool_id, num, kv.value);
    }
}
