// SPDX-License-Identifier: MIT
// Instantiate the real Rack module from the built plugin against libRack.
// No GUI, downloaded firmware, firmware fixture or proprietary SDK is needed.
#include <rack.hpp>
#include <dlfcn.h>
#include <unistd.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>

static void checkPatch(rack::engine::Module* module) {
    json_t* data = module->dataToJson();
    assert(json_is_object(data));
    const std::set<std::string> allowed{
        "schema", "firmwareSha256", "stateId", "clockMhz", "leadFrames"};
    const char* key;
    json_t* value;
    json_object_foreach(data, key, value) {
        assert(allowed.count(key));
        if (!std::strcmp(key, "firmwareSha256") || !std::strcmp(key, "stateId")) {
            assert(json_is_string(value));
            assert(std::strlen(json_string_value(value)) == 64);
        } else assert(json_is_integer(value));
    }
    char* text = json_dumps(data, JSON_SORT_KEYS);
    assert(text && std::strlen(text) < 512);
    std::free(text);
    json_decref(data);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    char root[] = "/tmp/firmwarehost-rack-smoke-XXXXXX";
    assert(mkdtemp(root));
    rack::asset::userDir = root;
    rack::random::init();
    rack::Context context;
    rack::contextSet(&context);
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::cerr << dlerror() << '\n'; return 1; }
    auto init = reinterpret_cast<void (*)(rack::plugin::Plugin*)>(dlsym(library, "init"));
    assert(init);
    {
        rack::plugin::Plugin plugin;
        plugin.slug = "FirmwareHost";
        plugin.path = ".";
        init(&plugin);
        assert(plugin.models.size() == 1);
        auto* model = plugin.getModel("FM1Host");
        assert(model);
        // Repeat construction/destruction: worker shutdown must be reliable.
        for (int instance = 0; instance < 3; ++instance) {
            std::unique_ptr<rack::engine::Module> module(model->createModule());
            assert(module && module->outputs.size() == 2 && module->inputs.size() == 7);
            checkPatch(module.get());
            // Arbitrary patch payloads never turn into filesystem paths or blobs.
            json_t* hostile = json_pack("{s:i,s:s,s:s,s:s,s:s}",
                "schema", 1, "firmwareSha256", "../../outside", "stateId", "/etc/passwd",
                "firmware", "not-a-firmware", "flash", "not-a-snapshot");
            module->dataFromJson(hostile);
            json_decref(hostile);
            checkPatch(module.get());
            // Headless modules have no widget calling step()/readStatus().
            for (float rate : {44100.f, 48000.f, 96000.f, 192000.f}) {
                rack::engine::Module::ProcessArgs args{};
                args.sampleRate = rate;
                args.sampleTime = 1.f / rate;
                module->inputs[0].setChannels(16);
                module->inputs[1].setChannels(16);
                for (int v = 0; v < 16; ++v) {
                    module->inputs[0].setVoltage((v - 8) / 12.f, v);
                    module->inputs[1].setVoltage(10.f, v);
                }
                for (int i = 0; i < 4000; ++i) {
                    args.frame = i;
                    module->process(args);
                    for (auto& port : module->outputs) {
                        assert(std::isfinite(port.getVoltage()));
                        assert(port.getVoltage() == 0.f);
                    }
                }
                module->inputs[1].setChannels(0);
                module->process(args);
            }
            checkPatch(module.get());
        }
    }
    rack::contextSet(nullptr);
    dlclose(library);
    // Only the empty cache directories created by this test are removed.
    const std::string cache = std::string(root) + "/FirmwareHost";
    rmdir((cache + "/firmware").c_str());
    rmdir((cache + "/state").c_str());
    rmdir(cache.c_str());
    rmdir(root);
    std::cout << "Real Rack module: lifecycle, 4 sample rates, 16 CV lanes, silent missing firmware, patch whitelist PASS\n";
}
