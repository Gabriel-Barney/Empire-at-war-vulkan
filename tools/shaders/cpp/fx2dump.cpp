// fx2dump.cpp - CLI test for fx2_reader: prints one summary line per effect so it can be diffed
// against the Python parser (tools/shaders/fx2.py) with check_cpp.py.
//   fx2dump file.fxo [file.fxo ...]
// Output: <file>\tparams=N\ttechs=N\tpasses=N\tstates=N\tshaders=N\tctab=N\tlods=a,b,c
#include "fx2_reader.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>

using namespace alamo::fx;

int main(int argc, char** argv) {
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        try {
            Effect e = parseEffect(d.data(), d.size());
            size_t passes = 0, states = 0, ctab = 0;
            std::set<const std::vector<uint8_t>*> blobs;
            std::string lods;
            for (const auto& t : e.techniques) {
                lods += (lods.empty() ? "" : ",") + (e.techniqueLod(t).empty() ? std::string("-") : e.techniqueLod(t));
                passes += t.passes.size();
                for (const auto& p : t.passes) {
                    states += p.states.size();
                    for (const auto& s : p.states)
                        if (s.kind == State::Kind::Shader && !s.blob.empty()) blobs.insert(&s.blob);
                }
            }
            for (const auto& p : e.parameters)
                if (p.type.type == ParamType::VertexShader || p.type.type == ParamType::PixelShader)
                    for (uint32_t id : p.value.objects) {
                        auto it = e.objects.find(id);
                        if (it != e.objects.end() && !it->second.data.empty()) blobs.insert(&it->second.data);
                    }
            for (auto* b : blobs) {
                ShaderInfo si = inspectShader(b->data(), b->size());
                if (!si.endTokenFound) { std::fprintf(stderr, "%s: shader without END\n", argv[i]); rc = 1; }
                ctab += si.constants.size();
            }
            std::printf("%s\tparams=%zu\ttechs=%zu\tpasses=%zu\tstates=%zu\tshaders=%zu\tctab=%zu\tlods=%s\n",
                        argv[i], e.parameters.size(), e.techniques.size(), passes, states, blobs.size(),
                        ctab, lods.c_str());
        } catch (const std::exception& ex) {
            std::printf("%s\tERROR %s\n", argv[i], ex.what());
            rc = 1;
        }
    }
    return rc;
}
