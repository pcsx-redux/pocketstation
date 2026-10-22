// Savestate round trip. Boots a kernel docked, stops in the middle of a card read with one
// exchange in flight, and saves. The same continuation (the rest of the read, a sector write, a
// button press, some sleep) then runs three times: on the original device, on the original device
// after loading the state back, and on a second device that loads it. The card bytes, the LCD and
// the final state blob of the three runs must be identical.
//   make -C tests/savestate check     (OpenPSK, built from ../../openpsk)
// KERNEL=path overrides the kernel. The workload checks (the read and write answers) are written
// for OpenPSK's card handler and fail on the retail kernel; the round-trip checks pass on both.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pocketstation.h"

using PCSX::PocketStation::PocketStation;

namespace {

constexpr uint64_t kSecond = 3997696;  // ARM7 cycles per second at the boot clock
constexpr int kReadSector = 5;
constexpr int kWriteSector = 9;
constexpr int kSaveAt = 70;  // the read's exchange in flight when the state is saved

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

std::vector<uint8_t> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct Rec {
    std::vector<uint8_t> bytes;  // every reply byte, then a marker per acknowledge
    std::vector<uint8_t> lcd;    // VRAM snapshots
};

// Wait for the kernel to acknowledge the exchange in progress, then close it. Returns the reply.
uint8_t finishExchange(PocketStation& dev, Rec& rec) {
    bool acked = false;
    for (int i = 0; i < 400 && !acked; i++) {
        dev.runCycles(64);
        acked = dev.comAcked();
    }
    const uint8_t reply = dev.comTakeReply();
    dev.comEndTransfer();
    rec.bytes.push_back(reply);
    rec.bytes.push_back(acked ? 1 : 0);
    return reply;
}

uint8_t exchange(PocketStation& dev, uint8_t in, Rec& rec) {
    dev.comBeginTransfer(in);
    return finishExchange(dev, rec);
}

// The bytes the PS1 sends for a read of sector n: 0x81 0x52 0 0 MSB LSB, then zeros.
std::vector<uint8_t> readCommand(int n) {
    std::vector<uint8_t> c = {0x81, 0x52, 0x00, 0x00, uint8_t(n >> 8), uint8_t(n)};
    c.resize(6 + 4 + 128 + 2, 0x00);
    return c;
}

std::vector<uint8_t> writeCommand(int n, const uint8_t* data) {
    std::vector<uint8_t> c = {0x81, 0x57, 0x00, 0x00, uint8_t(n >> 8), uint8_t(n)};
    uint8_t chk = uint8_t(n >> 8) ^ uint8_t(n);
    for (int i = 0; i < 128; i++) {
        c.push_back(data[i]);
        chk ^= data[i];
    }
    c.push_back(chk);
    c.resize(c.size() + 3, 0x00);
    return c;
}

void select(PocketStation& dev) {
    dev.comSetSelected(true);
    dev.runCycles(2000);  // the kernel takes its FIQ at the select, before the first byte
}

void deselect(PocketStation& dev) {
    dev.comSetSelected(false);
    dev.runCycles(20000);
}

std::vector<uint8_t> writeData() {
    std::vector<uint8_t> d(128);
    for (int i = 0; i < 128; i++) d[i] = uint8_t(0xA5 ^ (i * 37));
    return d;
}

// Everything after the save point: the read's remaining exchanges, a write, a button, sleep.
Rec continuation(PocketStation& dev) {
    Rec rec;
    const auto read = readCommand(kReadSector);
    finishExchange(dev, rec);  // the exchange that was in flight at the save
    for (size_t i = kSaveAt + 1; i < read.size(); i++) exchange(dev, read[i], rec);
    deselect(dev);
    rec.lcd.assign(dev.vram(), dev.vram() + 128);

    const auto data = writeData();
    const auto write = writeCommand(kWriteSector, data.data());
    select(dev);
    for (uint8_t b : write) exchange(dev, b, rec);
    deselect(dev);

    dev.setButtons(1);
    dev.runCycles(kSecond / 8);
    dev.setButtons(0);
    dev.runCycles(2 * kSecond + 12345);
    rec.lcd.insert(rec.lcd.end(), dev.vram(), dev.vram() + 128);
    return rec;
}

size_t firstDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return i;
    }
    return a.size() == b.size() ? std::string::npos : n;
}

bool same(const char* what, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t d = firstDiff(a, b);
    if (d == std::string::npos) return true;
    std::printf("  %s: first difference at byte %zu of %zu/%zu", what, d, a.size(), b.size());
    if (d < a.size() && d < b.size()) std::printf(" (%02X vs %02X)", a[d], b[d]);
    std::printf("\n");
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <kernel.bin>\n", argv[0]);
        return 2;
    }
    const auto kernel = readFile(argv[1]);
    if (kernel.size() != 16384) {
        std::fprintf(stderr, "%s: expected a 16 KiB kernel, got %zu bytes\n", argv[1], kernel.size());
        return 2;
    }
    std::vector<uint8_t> flash(128 * 1024);
    for (size_t i = 0; i < flash.size(); i++) flash[i] = uint8_t((i * 131) ^ (i >> 7));

    PocketStation a;
    a.setKernel(kernel.data(), kernel.size());
    a.setFlash(flash.data(), flash.size());
    a.reset();
    a.setDocked(true);
    a.runCycles(kSecond);

    // Up to the save point: the first kSaveAt exchanges of the read, then the next byte goes in
    // and the device runs for a while without the exchange being closed.
    Rec prefix;
    const auto read = readCommand(kReadSector);
    select(a);
    for (int i = 0; i < kSaveAt; i++) exchange(a, read[i], prefix);
    a.comBeginTransfer(read[kSaveAt]);
    a.runCycles(3);
    const bool inFlight = !a.comAcked();
    const auto saved = a.saveState();
    std::printf("state blob: %zu bytes, saved at instruction %llu\n", saved.size(),
                (unsigned long long)a.instructionsExecuted());

    // Positive controls on the workload itself, so a pass cannot come from a kernel that never
    // answered: the read so far carried the 5A 5D id-ack and the device is mid-exchange.
    check(prefix.bytes.size() >= 8 && prefix.bytes[4] == 0x5A && prefix.bytes[6] == 0x5D,
          "kernel answered the read command (5A 5D)");
    check(inFlight, "state saved with an exchange in flight (not yet acknowledged)");

    const Rec runA = continuation(a);
    const auto endA = a.saveState();

    check(a.loadState(saved.data(), saved.size()), "loadState accepts its own blob (same device)");
    const Rec runA2 = continuation(a);
    const auto endA2 = a.saveState();

    PocketStation b;
    std::vector<uint8_t> otherFlash(128 * 1024, 0xFF);
    b.setKernel(kernel.data(), kernel.size());
    b.setFlash(otherFlash.data(), otherFlash.size());
    b.reset();
    b.runCycles(kSecond / 3);
    check(b.loadState(saved.data(), saved.size()), "loadState accepts the blob (second device)");
    check(same("save after load", saved, b.saveState()), "save(load(blob)) == blob");
    const Rec runB = continuation(b);
    const auto endB = b.saveState();

    // The continuation did real work: the read returned the sector, the write was acknowledged
    // and landed in flash.
    // runA.bytes holds (reply, acked) per exchange, from the one in flight at the save onwards.
    const auto replyOf = [&](const Rec& r, size_t exchangeIndex) { return r.bytes[2 * (exchangeIndex - kSaveAt)]; };
    bool readOk = true;
    const int firstData = 10;  // 81 52 00 00 MSB LSB -> FF FLAG 5A 5D 00 00 5C 5D MSB LSB, then data
    for (int i = kSaveAt; i < firstData + 128; i++) {
        if (replyOf(runA, i) != flash[kReadSector * 128 + (i - firstData)]) readOk = false;
    }
    check(readOk, "continuation: read data matches the flash sector");
    check(replyOf(runA, read.size() - 1) == 0x47, "continuation: read ended with 0x47");
    const auto data = writeData();
    check(std::memcmp(b.flashData() + kWriteSector * 128, data.data(), 128) == 0,
          "continuation: the write landed in flash");
    check(a.instructionsExecuted() > 0 && runA.lcd.size() == 256, "continuation: LCD sampled");

    check(same("card bytes A/A'", runA.bytes, runA2.bytes), "card bytes: original == reloaded (same device)");
    check(same("card bytes A/B", runA.bytes, runB.bytes), "card bytes: original == reloaded (second device)");
    check(same("lcd A/A'", runA.lcd, runA2.lcd) && same("lcd A/B", runA.lcd, runB.lcd), "LCD identical");
    check(same("end state A/A'", endA, endA2), "end state: original == reloaded (same device)");
    check(same("end state A/B", endA, endB), "end state: original == reloaded (second device)");

    // Every byte loadState reads must reach the device. The comparisons above only see a field
    // whose value at the save point differs from its reset value, so flip bit 0 of each byte in
    // turn (header excluded), load and save again: the flip must come back out.
    {
        std::vector<uint8_t> probe = saved;
        size_t lost = 0, firstLost = 0, lastLost = 0;
        for (size_t k = 8; k < probe.size(); k++) {
            probe[k] ^= 1;
            const bool loaded = b.loadState(probe.data(), probe.size());
            const auto back = b.saveState();
            if (!loaded || back[k] != probe[k]) {
                if (lost++ == 0) firstLost = k;
                lastLost = k;
                if (lost <= 8) std::printf("  byte %zu: flip not restored\n", k);
            }
            probe[k] ^= 1;
        }
        std::printf("  %zu of %zu bytes did not survive load+save", lost, probe.size() - 8);
        if (lost) std::printf(" (first %zu, last %zu)", firstLost, lastLost);
        std::printf("\n");
        check(lost == 0, "every state byte survives a flip through load+save");
    }

    // A blob this build did not write is refused and changes nothing.
    const auto before = b.saveState();
    std::vector<uint8_t> bad = saved;
    bad.pop_back();
    const bool shortRefused = !b.loadState(bad.data(), bad.size());
    bad = saved;
    bad[0] ^= 1;
    const bool magicRefused = !b.loadState(bad.data(), bad.size());
    bad = saved;
    bad[4] ^= 1;
    const bool versionRefused = !b.loadState(bad.data(), bad.size());
    check(shortRefused && magicRefused && versionRefused, "bad blobs refused (length, magic, version)");
    check(same("after refusals", before, b.saveState()), "a refused blob leaves the device untouched");

    std::printf("%d failing check(s)\n", failures);
    return failures ? 1 : 0;
}
