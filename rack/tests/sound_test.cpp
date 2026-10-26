// Contract validation and lock restoration, independent of Rack's UI.
#include "../src/engine/BridgeJson.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <cstring>
using namespace gorgon;
static json_t* load(const char* s) {json_error_t e; auto* j=json_loads(s,JSON_DECODE_ANY,&e);assert(j);return j;}
int main() {
    Score score;
    score.sounds[0].machine=16;score.sounds[0].mask=2;score.sounds[0].values[1]=50;
    auto& lock=score.patterns[0].locks[0][0];lock.machine=48;lock.mask=3;lock.values[0]=0;lock.values[1]=127;
    auto a=soundAt(score,0,0,0),b=soundAt(score,0,0,1);
    assert(a.machine==48&&a.mask==3&&a.values[0]==0&&a.values[1]==127);
    assert(b.machine==16&&b.mask==2&&b.values[1]==50);
    for (const char* s : {"null","[]","{\"wat\":1}","{\"p0\":1}","{\"p1\":true}","{\"p1\":128}","{\"p1\":1.5}","{\"machine\":null}","{\"machine\":192}"}) {
        auto* j=load(s);gorgon_sound::Sound c;assert(!parseSoundParams(j,c,true));json_decref(j);
    }
    auto* j=load("{\"machine\":191,\"p1\":0,\"p8\":127.0}");gorgon_sound::Sound c;
    assert(parseSoundParams(j,c,true)&&c.machine==191&&c.mask==129&&c.values[7]==127);json_decref(j);
    // A complete document, including the version boundary.
    auto* doc=json_object();json_object_set_new(doc,"format",json_string("hydrapy-tracker-2"));
    auto* patterns=json_array();for(int i=0;i<4;++i)json_array_append_new(patterns,json_object());json_object_set_new(doc,"patterns",patterns);
    assert(!parseScore(doc,score));auto* sounds=json_array();for(int i=0;i<4;++i)json_array_append_new(sounds,load("{\"machine\":null,\"params\":{}}"));json_object_set_new(doc,"sounds",sounds);
    assert(parseScore(doc,score));assert(score.sounds[0].mask==0&&score.sounds[0].machine==-1);
    json_object_set_new(doc,"format",json_string("hydrapy-tracker-1"));assert(!parseScore(doc,score));
    json_object_del(doc,"sounds");assert(parseScore(doc,score));json_decref(doc);
    // Parse actual nested hit locks, including the version-1 rejection boundary.
    doc=json_object();json_object_set_new(doc,"format",json_string("hydrapy-tracker-2"));
    sounds=json_array();for(int i=0;i<4;++i)json_array_append_new(sounds,load("{\"machine\":16,\"params\":{\"p2\":50}}"));json_object_set_new(doc,"sounds",sounds);
    patterns=json_array();for(int p=0;p<4;++p){auto* pattern=json_object();auto* audio=json_array();for(int lane=0;lane<4;++lane){auto* rows=json_array();for(int r=0;r<64;++r)json_array_append_new(rows,json_null());json_array_append_new(audio,rows);}json_object_set_new(pattern,"audio",audio);json_array_append_new(patterns,pattern);}json_object_set_new(doc,"patterns",patterns);
    auto* rows=json_array_get(json_object_get(json_array_get(patterns,0),"audio"),0);
    auto* hit=load("{\"note\":\"C-4\",\"vel\":100,\"locks\":{\"machine\":48,\"p1\":0,\"p2\":127}}");json_array_set_new(rows,0,hit);
    assert(parseScore(doc,score));a=soundAt(score,0,0,0);b=soundAt(score,0,0,1);
    assert(a.machine==48&&a.mask==3&&a.values[0]==0&&a.values[1]==127&&b.machine==16&&b.mask==2&&b.values[1]==50);
    json_object_set_new(json_object_get(hit,"locks"),"p1",json_true());assert(!parseScore(doc,score));
    json_object_set_new(json_object_get(hit,"locks"),"p1",json_integer(0));
    json_object_set_new(doc,"format",json_string("hydrapy-tracker-1"));json_object_del(doc,"sounds");assert(!parseScore(doc,score));json_decref(doc);
    gorgon_sound::Packet p;assert(!gorgon_sound::valid(p,0));p.frame=8;assert(gorgon_sound::valid(p,10));
    assert(!gorgon_sound::valid(p,7)&&!gorgon_sound::valid(p,11));p.lanes[0].sound=c;
    assert(gorgon_sound::valid(p,9));p.lanes[0].sound.values[7]=std::numeric_limits<float>::quiet_NaN();assert(!gorgon_sound::valid(p,9));
    std::cout<<"sound_test: passed\n";
}
