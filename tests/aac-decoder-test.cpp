// SPDX-License-Identifier: GPL-2.0-or-later
// Links the actual patched Vita3K aac.cpp/decoder.cpp, with only logging isolated.
#include <codec/state.h>
#include <fstream>
#include <vector>
#include <iterator>
#include <cstdio>
#include <string>
int main(int argc, char** argv) {
    if (argc < 2) return 1;
    std::ifstream input(argv[1]);
    std::string hex;
    input >> hex;
    if (hex.empty() || hex.size()%2) return 2;
    std::vector<unsigned char> bytes;
    for (std::size_t i=0;i<hex.size();i+=2)
        bytes.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i,2),nullptr,16)));
    AacDecoderState decoder(44100,2);
    const unsigned expected[] = {133,230,177,135,7};
    unsigned offset=0;
    std::vector<unsigned char> output;
    for (auto length:expected) {
        if (!decoder.send(bytes.data()+offset,static_cast<unsigned>(bytes.size())-offset)) return 3;
        if (decoder.get_es_size()!=length) return 4;
        std::vector<unsigned char> pcm(8192);
        DecoderSize size{};
        if (!decoder.receive(pcm.data(),&size) || size.samples!=1024) return 5;
        output.insert(output.end(),pcm.begin(),pcm.begin()+size.samples*2*sizeof(short));
        offset+=length;
    }
    if (offset!=bytes.size()) return 6;
    unsigned char invalid=0;
    if (decoder.send(&invalid,1) || decoder.get_es_size()!=0 || decoder.send(nullptr,0)) return 7;
    if (argc>=3) {
        std::ifstream reference(argv[2],std::ios::binary);
        std::vector<unsigned char> pcm((std::istreambuf_iterator<char>(reference)),{});
        if (pcm!=output) return 8;
    }
    std::printf("PASS: five raw AAC frames, exact byte consumption, %zu PCM bytes, invalid-input rejection.\n",output.size());
}
