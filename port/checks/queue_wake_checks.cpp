#include "../compat/queue_wake.h"
#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include "spu_context.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

extern "C" {
extern uint8_t* vm_base;
uint32_t vm_read32(uint64_t);
void vm_write32(uint64_t,uint32_t);
void port_register_queue_api();
void ps3_hle_call(uint32_t,ppu_context*);
void spu_wrch(spu_context*,uint32_t,u128);
void spu_mfc_release(spu_context*);
extern void (*g_spu_line_commit_hook)(uint32_t);
extern uint64_t port_queue_wait_signals;
}
static unsigned legacy_notifications;
static void legacy_hook(uint32_t) { ++legacy_notifications; }

extern "C" int port_check_queue_wake() {
    using namespace std::chrono_literals;
    auto registry=std::make_unique<PortQueueWakeRegistry>();
    auto* a=registry->get(0x50000);
    auto* b=registry->get(0x50080);
    auto ticket=a->snapshot();
    bool empty=!a->wait(ticket,0ms);
    registry->publish(0x50080);
    bool unrelated=!a->wait(ticket,0ms);
    // Publication in the check-to-sleep gap must invalidate the saved ticket.
    registry->publish(0x50000);
    bool before_sleep=a->wait(ticket,0ms);
    ticket=a->snapshot();
    std::atomic<bool> entered{false},awoke{false};
    std::thread parked([&] { entered=true; awoke=a->wait(ticket,2000ms); });
    while(!entered.load()) std::this_thread::yield();
    registry->publish(0x50000);
    parked.join();
    bool bounded=!registry->get(0) && !registry->get(1) && !registry->publish(0);
    for(unsigned i=2;i<128;i++) bounded=registry->get(0x50000+128*i)!=nullptr && bounded;
    bounded=bounded && registry->get(0x90000)==nullptr && registry->get(0x50000)==a && b;

    constexpr uint32_t q=0x52000,out=0x52100,buffer=0x52200;
    memset(vm_base+q,0,128);memset(vm_base+out,0xA5,16);
    vm_write32(q+0x10,16);vm_write32(q+0x14,2);vm_write32(q+0x1C,buffer);
    auto* live=port_queue_wakes().get(q);
    auto spu=std::make_unique<spu_context>();spu_context_init(spu.get(),0);
    spu->mfc_lsa=0x1000;spu->mfc_eal=q;
    auto command=[&](uint32_t cmd) { u128 v{};v._u32[0]=cmd;spu_wrch(spu.get(),MFC_Cmd,v); };
    auto old_hook=g_spu_line_commit_hook;g_spu_line_commit_hook=legacy_hook;
    auto word=[&](unsigned off,uint32_t value) {
        for(unsigned i=0;i<4;i++) spu->ls[0x1000+off+i]=(uint8_t)(value>>(24-8*i));
    };
    ticket=live->snapshot();command(MFC_GETLLAR_CMD);word(4,1);command(MFC_PUTLLC_CMD);
    bool conditional=!spu->atomic_stat && live->wait(ticket,0ms) && vm_read32(q+4)==1;
    ticket=live->snapshot();command(MFC_PUTLLC_CMD);
    bool failed=spu->atomic_stat && !live->wait(ticket,0ms);
    ticket=live->snapshot();word(4,2);command(MFC_PUTLLUC_CMD);
    bool unconditional=live->wait(ticket,0ms) && vm_read32(q+4)==2;
    ticket=live->snapshot();word(4,3);command(MFC_PUTQLLUC_CMD);
    unconditional=live->wait(ticket,0ms) && unconditional && vm_read32(q+4)==3;
    bool legacy=legacy_notifications==3;g_spu_line_commit_hook=old_hook;

    // Exercise the actual HLE consumer with real guest payload, including an
    // unrelated notification while empty. Only the later MFC commit may finish.
    vm_write32(q,0);vm_write32(q+4,0);
    for(unsigned i=0;i<32;i++) vm_base[buffer+i]=(uint8_t)(i+1);
    port_register_queue_api();ppu_context ctx{};ctx.gpr[1]=0x60000;
    auto pop=[&](bool block) {
        ctx.gpr[3]=q;ctx.gpr[4]=out;ctx.gpr[5]=0;ctx.gpr[6]=block;
        ps3_hle_call(ps3_compute_nid("cellSpursQueuePopBody"),&ctx);
    };
    pop(false);bool nonblocking=(uint32_t)ctx.gpr[3]==0x80410901;
    for(unsigned i=0;i<16;i++) nonblocking=nonblocking && vm_base[out+i]==0xA5;
    std::atomic<bool> done{false};
    auto signals=std::atomic_ref<uint64_t>(port_queue_wait_signals).load();
    std::thread consumer([&] { pop(true);done=true; });
    std::this_thread::sleep_for(20ms);port_queue_wakes().publish(q);
    std::this_thread::sleep_for(20ms);bool spurious=!done.load();
    command(MFC_GETLLAR_CMD);word(4,1);command(MFC_PUTLLC_CMD);
    consumer.join();
    bool payload=!ctx.gpr[3] && vm_read32(q)==1 && !memcmp(vm_base+out,vm_base+buffer,16);
    bool integrated=std::atomic_ref<uint64_t>(port_queue_wait_signals).load()>signals;
    spu_mfc_release(spu.get());
    bool ok=empty && unrelated && before_sleep && awoke && bounded && conditional && failed && unconditional && legacy && nonblocking && spurious && payload && integrated;
    fprintf(stderr,"[compat-check] queue wake empty=%d unrelated=%d before-sleep=%d parked=%d bounded=%d PUTLLC=%d failed=%d PUTLLUC=%d legacy=%d nonblock=%d spurious=%d payload=%d integrated=%d %s\n",empty,unrelated,before_sleep,awoke.load(),bounded,conditional,failed,unconditional,legacy,nonblocking,spurious,payload,integrated,ok?"PASS":"FAIL");
    return ok?0:1;
}
