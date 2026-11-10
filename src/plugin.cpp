// SPDX-License-Identifier: MIT
#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    p->addModel(modelFM1Host);
}
