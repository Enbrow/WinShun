#pragma once

// Turns text into vectors through an OpenAI-compatible /embeddings endpoint:
// a cloud service (OpenAI, 阿里云百炼, 硅基流动…) or one on this computer
// (Ollama, LM Studio). The user picks the service and the model in the
// plugin's settings; nothing is sent until they did.

#include "winshun.h"

#include <string>
#include <vector>

namespace semantic {

struct Provider {
    std::string id; // none | ollama | siliconflow | bailian | openai | custom
    std::string name; // for the user: "Ollama（本机）"
    std::string endpoint; // ".../v1", without "/embeddings"
    std::string model;
    std::string key;
    std::string queryPrefix; // what the model wants before a query…
    std::string docPrefix; // … and before a passage
    bool local = false; // runs on this computer: nothing leaves it, but it takes the CPU
    int batch = 10; // texts per request

    bool configured() const { return id != "none" && !endpoint.empty() && !model.empty() && (local || !key.empty()); }
    // What the stored vectors depend on: another one means indexing again.
    std::string signature() const;
};

Provider providerFrom(const winshun::Json& settings, bool chinese);

struct Embedded {
    std::vector<std::vector<float>> vectors; // normalized; one per text, in order
    std::string error; // for the user; empty when it worked
    bool retryLater = false; // the service is away or busy: the same texts may work later
    bool badInput = false; // the service refused these texts (too long…): skip them
};

// `query`: the texts are what the user typed (queryPrefix), else passages.
Embedded embed(const Provider& provider, const std::vector<std::string>& texts, bool query, bool chinese);

} // namespace semantic
