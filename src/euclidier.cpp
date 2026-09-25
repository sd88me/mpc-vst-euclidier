/*******************************************************************
 The 8 Track Euclidean Sequencer Main Program
 ***************************************************************** */
#include <mutex>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <string.h>
#include <condition_variable>
#include "eqseq.h"
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>
#include <iostream>
#include <sys/time.h>
#include <ctime>
#include <sstream>
#include "RtMidi.h"
#include <math.h>
#include <algorithm>
#include "commontypes.h"
using namespace std;
using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::system_clock;
void onMIDI(double deltatime, std::vector<unsigned char> *message, void * /*userData*/);
void sendNote(unsigned char type, unsigned char ch, unsigned char note, unsigned char vel);
void listInports();
uint getinPort(std::string str);
uint getOutPort(std::string str);
void doTempo();
void handleClockMessage(unsigned char message);
void pulse();
void updateBPM(float bpm);
void clockStart();
void clockContinue();
bool pendingSync = false;
long long getUS();
void sendTicks();
void clear();
void loadPatch(int slot);
void savePatch(int slot);
bool velSense = true;
bool receiveNotes = true;
bool autosync = true;
float BPM = 120.00;
int getOffset();
float syncDiv = 3;
bool doSync = false; // master-sync quantize (CC 50); off = edits apply immediately (safe: step position is stateless)
std::mutex engineMutex;
std::condition_variable engineCv; // wakes the main loop (event-driven: no polling)
bool engineWake = false;
struct WakeMain // set at scope exit of onMIDI: tell the main loop something changed
{
    ~WakeMain()
    {
        engineWake = true;
        engineCv.notify_one();
    }
}; // guards lane state: MIDI callback thread vs main loop
long long pendingTick = 0; // last song position (0xF2) in clock ticks
const string BANK = "BANK.bin";

void clockStop();
string typeLabel(int trk);
unsigned long long now();
unsigned long long ms();
unsigned long long BOOT_TIME;
unsigned long long sstep;                // sequence step
const bool CONNECT_AKAI_NETWORK = false; // automatically connevt to akai network remote port
const unsigned char STEPMAX = 64;        // number of max steps and pulses
const unsigned VEL_SENSE_MIN = 22;
const unsigned VEL_SENSE_MAX = 127;
unsigned char currSlot = 0;

long long tick = 0;

vector<double> pulses;
int limit(int v, int min, int max);
int RANDLANE = 0;
int RANDMAXFILL = 40;
bool paused = false;
bool started = false;
bool bgprocess = false;
bool extClock = true;
int bpm1 = 120;
int bpm2 = 0;
int intBPM = 120;
long long lastPulse = 0;
int SLEEP_UNIT = 5000; // 5ms
void printAll(bool _clear);
void printLane(int trk);
void resync(bool force, bool print);
void updateSteps(unsigned char trk, unsigned char VALUE);
void updatePulse(unsigned char trk, unsigned char VALUE);
void updateShift(unsigned char trk, unsigned char VALUE);
void updateLoop(unsigned char trk, unsigned char VALUE);
void updateDiv(unsigned char trk, unsigned char VALUE);
void updateNote(unsigned char trk, unsigned char VALUE);
void updateEnable(unsigned char trk, unsigned char VALUE);
int getRandValue(int lo, int hi);
void Randomize();
void RandomizeLane(int lane);

void createBANK(string filename);
void sendClock(unsigned char msg);
void setSleep();
string basePath = "";
EUPATCH seqPatch();
std::string
FW(std::string label, int value, int max_digits);
float getDiv(unsigned char d);
struct loadedPatch
{
    int slot = -1;
    EUPATCH patch;
};
loadedPatch loaded;
long long loading = 0;
bool saving = false;
RtMidiIn *midiIn = 0;
RtMidiIn *HWIN = 0;
RtMidiOut *midiOut = 0;
enum syncTypes
{
    STEP,
    PULSE,
    SHIFT,
    LOOP,
    DIV,
    NOTE,
    BV,
    VA,
    CH,
    GATE,
    ENABLE
};
bool needsSync(syncTypes T);
struct syncMessage
{
    unsigned char CC;
    unsigned char TRACK;
    unsigned char VALUE;
    syncTypes TYPE; // 0=step,1=pulse, 2 loop,3=div
};
void queCC(unsigned char CC, unsigned char TRK, unsigned char VALUE, syncTypes type);
vector<syncMessage> QUEUE;
vector<syncMessage> WAIT_QUEUE;
void processMessage(syncMessage M);
void processQ();
bool QPROCESSING = false;

const unsigned char SEQS = 8;

EQSEQ *SQ = new EQSEQ[8]; // creante the 8 track sequencer in an array
string ctrlSockPath = "/tmp/euclidier_ctrl.sock";
void startCtrlSocket();
int main(int argc, char *argv[])
{
    for (int a = 1; a < argc; a++)
    {
        if (string(argv[a]) == "-v")
            bgprocess = true;
        else if (string(argv[a]) == "--ctrl-sock" && a + 1 < argc)
            ctrlSockPath = argv[++a];
    }
    // || string(argv[0]) == "/media/662522/AddOns/nodeServer/modules/euclidier";

    srand(time(NULL));
    clear();
    char c[260];
    int l = (int)readlink("/proc/self/exe", c, 260);
    // cout << string(c, l > 0 ? l : 0) << endl;
    string tss = string(c, l > 0 ? l : 0);
    basePath = tss.length() > 0 ? tss.append(BANK) : "euclidier" + BANK;
    BOOT_TIME = now();
    midiIn = new RtMidiIn();
    midiIn->setCallback(&onMIDI);
    midiIn->ignoreTypes(false, false, false); // dont ignore clocK

    midiOut = new RtMidiOut();
    if (CONNECT_AKAI_NETWORK)
    {
        HWIN = new RtMidiIn();
        HWIN->setCallback(&onMIDI);
        HWIN->ignoreTypes(false, false, false); // dont ignore clocK

        int op = getOutPort("Akai Network - MIDI");
        if (op != 99)
        {

            midiOut->openPort(op);
            cout << "Opened Network output" << endl;
        }
        else
        {
            midiOut->openVirtualPort("Euclidier");
        }
        int dp = getinPort("Akai Network - MIDI");
        if (dp != 99)
        {
            delete midiIn;
            HWIN->openPort(dp);
            cout << "Opened Network input" << endl;
        }
    }
    else
    { // normal midi
        midiIn->openVirtualPort("Euclidier");
        midiOut->openVirtualPort("Euclidier");
    }

    if (!bgprocess)
        cout << "Ports opened - Waiting for Midi Clock Input to Start " << endl;

    for (int i = 0; i < SEQS; i++) // initialize sequencer parameters
    {
        SQ[i].setPORT(midiOut);

        if (i > 3)
        {
            SQ[i].ch = 10;
            SQ[i].mode = 2; // set 4-8 to drum type non transposeable.
        }

        if (i == 4)
        {
            SQ[i].note = 36;
        }

        if (i == 5)
        {
            SQ[i].note = 41;
            SQ[i].pulses = 2;
            SQ[i].shift = 4;
        }
        if (i == 6)
        {
            SQ[i].note = 38;
            SQ[i].steps = 4;
            SQ[i].pulses = 4;
        }
        if (i == 7)
        {
            SQ[i].note = 39;
            SQ[i].pulses = 4;
            SQ[i].shift = 2;
        }
        SQ[i].updateSeq();
    }
    SQ[0].ENABLE(true);
    createBANK(basePath);
    // SQ[0].pulses = 13;
    //  loadPatch(0);
    // savePatch(7);
    //  loadPatch(1);
    sleep(1);
    //    loadPatch(0);
    printAll(false);
    startCtrlSocket();
    long long last = 0;

    while (true) // the main loop
    {
        long long us = getUS();
        if (loading != 0 && us > loading) // patch recall expired
        {
            loading = 0;
        }
        std::unique_lock<std::mutex> lk(engineMutex);
        if (started)
        {
            if (!extClock) // generate pulse
            {
                float pls = (60000 * 1000) / (BPM * 24);
                long long pulseUS = round(pls);
                long long diff = us - lastPulse;
                if ((diff >= pulseUS) || lastPulse == 0)
                {
                    long correction = 0;
                    if (lastPulse > 0)
                    {
                        correction = pls - diff;
                        // float _bpm = (24 * diff)
                    }
                    BPM = intBPM;
                    pulse();
                    lastPulse = us + correction;
                }
            }

            if (!doSync)
                processQ();
            for (char i = 0; i < SEQS; i++)
            {
                SQ[i].clock(us);
            }
        }

        if (started && !extClock) // dormant internal clock needs the fine-grained poll
        {
            lk.unlock();
            usleep(SLEEP_UNIT);
            continue;
        }
        // Event-driven: sleep until the next pending note-off (or patch-recall expiry),
        // or indefinitely; onMIDI() wakes us for every incoming message.
        long long dl = 0;
        for (int i = 0; started && i < SEQS; i++) // note-offs are only processed while started
        {
            long long o = SQ[i].nextOff();
            if (o > 0 && (dl == 0 || o < dl))
                dl = o;
        }
        if (loading != 0 && (dl == 0 || loading < dl))
            dl = loading;
        if (!engineWake)
        {
            if (dl == 0)
                engineCv.wait(lk, [] { return engineWake; });
            else
            {
                long long w = dl - getUS();
                engineCv.wait_for(lk, std::chrono::microseconds(w < 50 ? 50 : w), [] { return engineWake; });
            }
        }
        engineWake = false;
    }

    return 0;
}
void printAll(bool _clear = true) // prints the sequence to console.
{
    if (bgprocess)
        return;
    if (_clear)
        clear();
    cout << string(80, '*') << endl;

    cout << "        <<<  SEQUENCES  >>>  " << endl;
    cout << string(80, '*') << endl;

    for (int i = 0; i < SEQS; i++)
    {
        cout << (SQ[i].enabled ? "* " : "  ")
             << typeLabel(i)
             << FW("1/", SQ[i].getDiv(), 2)
             << "  | ";

        SQ[i].print();
    }
    /*   cout << endl
            << "  *********************************" << endl
            << endl;*/
    cout << endl
         << string(80, '*') << endl;
    if (loaded.slot != -1)
    {
        cout << "        <<< Loaded Slot: " << loaded.slot << "  VALUES  >>>  " << endl;
        cout << string(80, '*') << endl;
        for (int i = 0; i < SEQS; i++)
        {
            printLane(i);
        }
        cout << endl
             << string(80, '*') << endl;
    }
}
void clear()
{
    if (bgprocess)
        return;
    // CSI[2J clears screen, CSI[H moves the cursor to top-left corner
    cout << "\x1B[2J\x1B[H";
    cout << "  ************************************" << endl;
    cout << "  Euclidier v1.9  (Press Ctrl + C to Quit)" << endl;
    cout << "  ************************************" << endl;
}

void handleMidi(std::vector<unsigned char> *message); // body below; caller holds engineMutex
void onMIDI(double deltatime, std::vector<unsigned char> *message, void * /*userData*/) // handles incomind midi
{
    std::lock_guard<std::mutex> lock(engineMutex);
    WakeMain wake;
    handleMidi(message);
}
void handleMidi(std::vector<unsigned char> *message) // shared by the MIDI callback and the control socket
{

    unsigned char byte0 = (int)message->at(0);
    unsigned char typ = byte0 & 0xF0;
    uint size = message->size();
    // cout << "message " << byte0 << endl;

    if (byte0 == 0xF2 && size >= 3) // song position pointer (units of 6 clocks)
    {
        pendingTick = ((long long)message->at(1) | ((long long)message->at(2) << 7)) * 6;
        if (started && extClock)
            tick = pendingTick; // locate while running
        return;
    }
    if (size == 1) // system realtime message
    {
        handleClockMessage(message->at(0));
        return;
    }
    if (loading != 0)
        return;
    if (typ == 0x90 && receiveNotes) // note message
    {

        unsigned char VAL = (int)message->at(2);
        if (VAL < 1) // disregard notes with velocity less than 10
            return;
        unsigned char note = (int)message->at(1);

        unsigned char oct = note / 12;
        unsigned char xpose = note % 12;
        if (oct < 8) // xpose tracks 1-8 {}
        {
            if (SQ[oct].mode > 1)
                return;

            unsigned targetCC = (oct * 10) + 2;
            unsigned ooct = 0;
            if (velSense)
            {
                ooct = VAL < VEL_SENSE_MIN ? -1 : 0;
                ooct = VAL >= VEL_SENSE_MAX ? 1 : ooct;
                SQ[oct].octave = ooct;
            }

            SQ[oct].xpose = xpose;
        }
        if (xpose < 8 && SQ[xpose].mode > 1)
            return;

        if (oct == 8 && xpose < 8) // 96-103
        {
            SQ[xpose].octave = 0;
        }
        if (oct == 9 && xpose < 8) // 108-115
        {

            SQ[xpose].octave = 1;
        }
        if (oct == 10 && xpose < 8) // 120-127
        {

            SQ[xpose].octave = -1;
        }
    }
    if (typ == 0xC0) // program change message
    {
        unsigned char PC = (int)message->at(1);
        // unsigned char VAL = (int)message->at(2);
        loadPatch(limit(PC, 0, 127));
        return;
    }
    if (typ == 0xB0) // cc message
    {

        // cout << "CC" << endl;
        /*
        params
        enabled: true/false
        TIMING : 1-5
        ch: 1-16
        steps: 4-STEMPMAX
        pulses: 2-STEPMAX
        shift : 0-32
        gate : 10-95%
        */

        unsigned char CC = (int)message->at(1);
        unsigned char VAL = (int)message->at(2);
        unsigned char ch = byte0 & 0x0F;
        unsigned char trk = CC / 10;
        unsigned char cmd = CC % 10;
        if (CC == 20)
        {
            currSlot = limit(VAL, 0, 127);
            return;
        }
        if (CC == 29 && VAL == 127)
        {
            loadPatch(currSlot);
            return;
        }
        if (CC == 30 && VAL == 127)
        {
            savePatch(currSlot);
            return;
        }

        if (CC == 39)
        { // BPM1
            bpm1 = limit(VAL, 30, 127);
            intBPM = bpm1 + bpm2;
        }
        if (CC == 40)
        { // BPM1
            bpm2 = limit(VAL, 0, 127);
            intBPM = bpm1 + bpm2;
            return;
        }
        if (CC == 59)
        {
            extClock = limit(VAL, 0, 1);

            return;
        }

        if (CC == 60) // start stop
        {
            if (extClock)
                return;
            if (VAL == 0 && started)
            { // steop

                clockStop();
            }
            if (VAL == 1 & !started)
            {
                clockStart();
            }

            return;
        }
        if (CC == 79)
        {
            RANDMAXFILL = VAL < 10 ? 10 : VAL;
            RANDMAXFILL = VAL > 100 ? 100 : VAL;
        }
        if (CC == 89)
        {
            VAL = VAL > 10 ? 10 : VAL;
            RANDLANE = VAL;
        }
        if (CC == 90 && VAL > 0 && VAL % 2 == 0)
        {
            loading = getUS() + (1000 * 100);
            if (RANDLANE > 0 && RANDLANE <= 8)
            {

                RandomizeLane(RANDLANE - 1);
            }
            else
            {
                for (int i = 0; i != 8; i++)
                {
                    if ((RANDLANE == 9 && i == 0) || RANDLANE == 10 && i == 4)
                    {
                        cout << "Lane: " << i + 1 << " skipped from randomization!" << endl;
                    }
                    else
                    { // for values 9 and 10, skips randomizing of lanes 1  or 5 (usually kick)
                        RandomizeLane(i);
                    }
                }
            }
            printAll(true);
        }

        if (CC == 100 && VAL > 0 && VAL % 2 == 0)
        {
            for (unsigned char i = 0; i < SEQS; i++)
            {

                SQ[i].octave = 0;
                SQ[i].xpose = 0;
            }

            return;
        }
        if (CC == 119)
        {
            bool wasActive = velSense;
            velSense = limit(VAL, 0, 1);
            if (!velSense && wasActive) // reset the octaves
            {
                for (unsigned char i = 0; i < SEQS; i++)
                {

                    SQ[i].octave = 0;
                }
            }

            return;
        }
        if (CC == 99) // receive notes
        {

            receiveNotes = limit(VAL, 0, 1);

            return;
        }
        if (CC == 80) // autosync
        {

            for (unsigned char i = 0; i < SEQS; i++)
            {

                SQ[i].autosync = limit(VAL, 0, 1);
            }

            return;
        }
        if (CC == 70) // resync
        {

            resync(true, true);

            return;
        }
        if (CC == 50) // master sync
        {
            if (VAL == 0)
            {
                doSync = false;

                return;
            }
            doSync = true;
            syncDiv = getDiv(limit(VAL, 1, 8));
        }

        if (trk < 8) // if sequncer track messages
        {
            switch (cmd)
            {
            case 1: // enabled
                if (!started)
                {
                    SQ[trk].ENABLE(VAL > 63);
                    printAll();
                    return;
                }
                if (SQ[trk].enabled != (VAL > 63))
                {
                    queCC(CC, trk, VAL, ENABLE);
                }
                break;

            case 2: // note
                if (!started)
                {
                    updateNote(trk, VAL);
                    // SQ[trk].updateSeq();
                    printAll();
                    return;
                }
                queCC(CC, trk, VAL, NOTE);
                break;

            case 3:
                //  SQ[trk].updateDiv(limit(VAL, 1, 10));
                if (!started)
                {
                    updateDiv(trk, VAL);
                    SQ[trk].updateSeq();
                    printAll();
                    return;
                }
                queCC(CC, trk, VAL, DIV);
                return;

                break;

            case 4:
                if (trk == 6) // ignore cc 64
                    break;
                if (!started)
                {
                    updateSteps(trk, VAL);
                    SQ[trk].updateSeq();
                    printAll();
                    return;
                }
                queCC(CC, trk, VAL, STEP);

                break;

            case 5:

                if (!started)
                {
                    updatePulse(trk, VAL);
                    SQ[trk].updateSeq();
                    printAll();
                    return;
                }
                queCC(CC, trk, VAL, PULSE);
                break;

            case 6:

                if (!started)
                {
                    updateShift(trk, VAL);
                    SQ[trk].updateSeq();
                    printAll();
                    return;
                }
                queCC(CC, trk, VAL, SHIFT);
                break;
            case 7:

                if (trk == 0) // ignore cc 64
                    break;

                SQ[trk].setGATE(limit(VAL, 10, 95));
                break;
            case 8:

                SQ[trk].updateCH(limit(VAL, 1, 15));
                printAll();
                break;

            case 9: // edge cases for force
                if (trk == 0)
                {
                    SQ[trk].setGATE(limit(VAL, 10, 95));

                    break;
                }
                if (trk == 6) // track 6 cc64
                {
                    if (!started)
                    {
                        updateSteps(trk, VAL);
                        SQ[trk].updateSeq();
                        printAll();
                        return;
                    }
                    queCC(CC, trk, VAL, STEP);
                    return;
                }
            }
        }

        else // trk > 7
        {
            if (trk == 8 && cmd > 0 && cmd <= 8) // velocity messages
            {
                SQ[cmd - 1].vel = VAL;
            }
            if (trk == 9 && cmd > 0 && cmd <= 8) // velocity humanization messages
            {
                SQ[cmd - 1].velh = VAL;
            }
            if (trk == 10 && cmd > 0 && cmd <= 8) // loop steps 1-64
            {
                //  cout << "loop" << endl;
                SQ[cmd - 1].loop = limit(VAL, 0, 64); // 0=off
                printAll();
            }
            if (trk == 11 && cmd > 0 && cmd <= 8) // set Modes 1-3
            {
                unsigned char wasMode = SQ[cmd - 1].mode;
                SQ[cmd - 1].setMode(VAL); // 0=off
                if (SQ[cmd - 1].mode == 2 && wasMode != 2)
                    SQ[cmd - 1].note = 35 + cmd; // entering drum mode: default note to this lane's own GM drum slot (lane 1 -> 36, ...)
                printAll();
            }
        }
    }
}

void sendNote(unsigned char type, unsigned char ch, unsigned char note, unsigned char vel)
{
    std::vector<unsigned char> messageOut;
    messageOut.push_back(ch + type);
    messageOut.push_back(note);
    messageOut.push_back(vel);
    // cout << "Sending " << (int)note << " with value " << (int)vel << endl;
    midiOut->sendMessage(&messageOut);
}
void sendClock(unsigned char msg)
{
    std::vector<unsigned char> messageOut;
    messageOut.push_back(msg);
    midiOut->sendMessage(&messageOut);
}
int limit(int v, int min, int max)
{
    if (v < min)
        v = min;
    if (v > max)
        v = max;
    return v;
}
void resync(bool force, bool print) // after parameter update , set all sequences to step 0 so they are in sync
{
    for (char i = 0; i < SEQS; i++)
    {

        SQ[i].reset(force);
    }
    if (print)
        printAll();
    paused = true;
}

void listInports()
{
    uint nPorts = HWIN->getPortCount();
    for (uint i = 0; i < nPorts; i++)
    {
        std::string portName = HWIN->getPortName(i);

        std::cout << "Port: " << i << " = " << portName << "\n";
    }
}
uint getinPort(std::string str)
{
    uint nPorts = HWIN->getPortCount();
    for (uint i = 0; i < nPorts; i++)
    {
        std::string portName = HWIN->getPortName(i);
        size_t found = portName.find(str);
        if (found != string::npos)
        {
            return i;
        }
    }
    return 99;
}
uint getOutPort(std::string str)
{
    uint nPorts = midiOut->getPortCount();
    for (uint i = 0; i < nPorts; i++)
    {
        std::string portName = midiOut->getPortName(i);
        size_t found = portName.find(str);
        if (found != string::npos)
        {
            return i;
        }
    }
    return 99;
}

void handleClockMessage(unsigned char message)
{

    // cout << "message: " << (int)message << endl;
    switch (message)
    {

    case 248:
        if (extClock)
            pulse();
        return;
    case 250:
        if (extClock)
            clockStart();
        return;
    case 251: // continue: resume from the last song position
        if (extClock && !started)
            clockContinue();
        return;
    case 252:
        clockStop();
        return;
    }
}

void pulse() // used to compute bpm and send clock message to sequencer for sync
{
    if (!extClock)
    {
        sendClock(248);
    }
    sendTicks();
    tick += 1;
    sstep++;

    int ystep = tick == 0 ? 0 : (tick + 1) % (int)((24 * 4 / syncDiv));
    if (ystep == 0 && doSync)
        processQ();
    if (extClock)
    {
        const uint mSize = 12; // fill up averaging buffer before computing final bpm;
        pulses.push_back(ms());
        /* if (pulses.size() < mSize)
             return;*/
        if (pulses.size() > mSize)
        {
            pulses.erase(pulses.begin());
        }

        double avg = 0;
        unsigned cnt = 0;
        for (uint i = 1; i < pulses.size(); i++)
        {
            double msd = pulses.at(i) - pulses.at(i - 1);
            double localavg = (60 / ((msd * 24) / 1000));
            avg += localavg;
            cnt++;
        }
        float bpm = (avg / cnt);
        BPM = bpm;
    }

    updateBPM(BPM);
}
void clockContinue()
{
    tick = pendingTick;
    lastPulse = 0;
    started = true;
    setSleep();
}
void clockStart()
{
    pendingTick = 0;

    if (!bgprocess)
        std::cout << "Clock Started"
                  << "\n"
                  << std::flush;
    if (!extClock)
        sendClock(250);
    tick = 0;
    sstep = 0;
    lastPulse = 0;
    resync(true, true);
    started = true;
    setSleep();
}
void clockStop()
{
    if (!extClock)
        sendClock(252);
    if (!bgprocess)
        std::cout << "Clock Stopped"
                  << "\n"
                  << std::flush;
    started = false;
    setSleep();
    resync(true, false);
}

unsigned long long now() // current time since epoch in ms
{
    return (unsigned long long)duration_cast<milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count();
}

unsigned long long ms() // ms elapsed since app started.
{
    return now() - BOOT_TIME;
}
void updateBPM(float bpm) // sends the computed bpm to all sequencers
{
    if (isinf(bpm))
        return;

    for (char i = 0; i < SEQS; i++)
    {
        SQ[i].setBPM(bpm);
    }
}
void sendTicks() // send midi pulse to all sequencers
{
    long long us = getUS();

    for (int i = 0; i < SEQS; i++)
    {

        SQ[i].tick(tick, us);
    }
}

long long getUS() // gets time since epch in microseconds
{
    auto t1 = std::chrono::system_clock::now();
    long long us = duration_cast<microseconds>(t1.time_since_epoch()).count();
    return us;
}

void queCC(unsigned char CC, unsigned char TRK, unsigned char VALUE, syncTypes type)
{
    int index = -1;
    syncMessage M = {.CC = CC, .TRACK = TRK, .VALUE = VALUE, .TYPE = type};

    if (!QPROCESSING)
    {
        for (vector<syncMessage>::size_type i = 0; i != QUEUE.size(); i++)
        {
            if (QUEUE.at(i).CC == CC)
            {
                QUEUE.at(i).VALUE = VALUE;
                return;
            }
        }

        QUEUE.push_back(M);
        return;
    }
    else // wait que
    {
        for (vector<syncMessage>::size_type i = 0; i != WAIT_QUEUE.size(); i++)
        {
            if (WAIT_QUEUE.at(i).CC == CC)
            {
                WAIT_QUEUE.at(i).VALUE = VALUE;
                return;
            }
        }
        WAIT_QUEUE.push_back(M);
        return;
    }
}

void updateSteps(unsigned char trk, unsigned char VALUE)
{
    SQ[trk].steps = limit(VALUE, 2, STEPMAX);
}
void updateEnable(unsigned char trk, unsigned char VALUE)
{

    SQ[trk].ENABLE(VALUE > 63);
}
void updateNote(unsigned char trk, unsigned char VALUE)
{
    SQ[trk].note = limit(VALUE, 0, 127);
}
void updatePulse(unsigned char trk, unsigned char VALUE)
{
    SQ[trk].pulses = limit(VALUE, 1, STEPMAX);
}
void updateShift(unsigned char trk, unsigned char VALUE)
{
    SQ[trk].shift = limit(VALUE, 0, STEPMAX);
}
void updateLoop(unsigned char trk, unsigned char VALUE) {}
void updateDiv(unsigned char trk, unsigned char VALUE)
{
    SQ[trk].updateDiv(limit(VALUE, 1, 10));
}
void processQ()
{
    QPROCESSING = true;
    vector<unsigned char> updated;
    for (vector<syncMessage>::size_type i = 0; i != QUEUE.size(); i++)
    {
        processMessage(QUEUE.at(i));
        if (needsSync(QUEUE.at(i).TYPE)) // to not resync on enable disable
            updated.push_back(QUEUE.at(i).TRACK);
    }
    QUEUE.clear();
    QPROCESSING = false;
    for (vector<syncMessage>::size_type i = 0; i != WAIT_QUEUE.size(); i++)
    {
        processMessage(WAIT_QUEUE.at(i));
        if (needsSync(WAIT_QUEUE.at(i).TYPE))
            updated.push_back(WAIT_QUEUE.at(i).TRACK);
    }
    WAIT_QUEUE.clear();
    if (updated.size())
    {
        for (unsigned char i = 0; i != 8; i++)
        {
            if (std::find(updated.begin(), updated.end(), i) != updated.end())
            {
                SQ[i].updateSeq();
            }
        }

        // SQ[trk].updateSeq();
        resync(false, true);
        // printAll();
        //  send sync messages
    }
}
void processMessage(syncMessage M)
{
    switch (M.TYPE)
    {
    case STEP:
        updateSteps(M.TRACK, M.VALUE);
        break;
    case PULSE:
        updatePulse(M.TRACK, M.VALUE);
        break;

    case LOOP:
        updateLoop(M.TRACK, M.VALUE);
        break;

    case SHIFT:
        updateShift(M.TRACK, M.VALUE);
        break;

    case DIV:
        updateDiv(M.TRACK, M.VALUE);
        break;
    case NOTE:
        updateNote(M.TRACK, M.VALUE);
        break;
    case ENABLE:
        updateEnable(M.TRACK, M.VALUE);
        break;
    }
}

float getDiv(unsigned char d) // return midi time division 1/16,1/4 etc based on midi received valiees 1-6
{
    float div = 16.0;
    switch (d)
    {
    case 0:
    case 1:
        div = 16.0;
        break;
    case 2:
        div = 8.0;
        break;

    case 3:
        div = 4.0;
        break;

    case 4:
        div = 2.0;
        break;

    case 5:
        div = 1.0;
        break;

    case 6:
        div = 1.0 / 2;
        break;
    case 7:
        div = 1.0 / 4;
        break;
    case 8:
        div = 1.0 / 8;
        break;
    }
    return div;
}

void printLane(int trk)
{
    //   lanePatch p = SQ[trk].getPatch();
    lanePatch p = loaded.patch.lane[trk];

    std::stringstream S;
    S << trk + 1 << "."
      << FW(" E:", (int)p.enabled, 1)

      << FW(" S:", (int)p.steps, 2)

      << FW(" F:", (int)p.pulses, 2) << FW(" >:", (int)p.shift, 2)
      << FW(" L:", (int)p.loop, 2) << FW(" D:", (int)p.div, 2)

      << FW(" N:", (int)p.note, 3) << FW(" G:", (int)p.gate, 2)
      << FW(" BV:", (int)p.BV, 3) << FW(" VA:", (int)p.VA, 3)
      << FW(" T:", (int)p.type, 1) << FW(" Ch:", (int)p.ch, 2);
    cout << S.str() << endl;
}
std::string FW(std::string label, int value, int max_digits)
{

    string s = label.append(to_string(value));

    int spaces = max_digits - to_string(value).length();
    if (spaces > 0)
    {
        string ss = string(spaces, ' ');

        s.append(ss);
    }
    return s;
}
void createBANK(string filename)
{
    /*  cout << "BANK: " << sizeof(EUBANK)
           << " PATCH:" << sizeof(EUPATCH)
           << " Lane:" << sizeof(lanePatch);*/
    struct stat buffer;
    /*cout << " Creating " << filename << endl;*/
    if (stat(filename.c_str(), &buffer) == 0) // should be 0
    {
        //    cout << "exists" << endl;
    }
    else
    {
        //   cout << " notexists" << endl;

        EUPATCH E = seqPatch();

        FILE *o;
        o = fopen(filename.c_str(), "wb");
        if (o != NULL)
        {
            for (int i = 0; i != 128; i++)
            {
                fwrite(&E, sizeof(struct EUPATCH), 1, o);
            }
            fclose(o);
            //  cout << " Written" << endl;
        }
    }
}
EUPATCH seqPatch()
{
    EUPATCH pch;
    for (int i = 0; i != 8; i++)
    {
        lanePatch l = SQ[i].getPatch();
        pch.lane[i] = l;
    }

    return pch;
}

void loadPatch(int slot)
{

    FILE *F;
    slot = limit(slot, 0, 127);

    F = fopen(basePath.c_str(), "rb");
    if (F != NULL)
    {
        EUPATCH E;
        fseek(F, sizeof(struct EUPATCH) * slot, SEEK_SET);
        fread(&E, sizeof(struct EUPATCH), 1, F);
        fclose(F);
        loaded.patch = E;
        loaded.slot = slot;
        loading = getUS() + (1000 * 100); // 100ms
        // cout << "loading is" << loading << endl;
        for (int i = 0; i != 8; i++)
        {

            SQ[i].setMode(E.lane[i].type);
            sendNote(0xB0, 15, i + 111, E.lane[i].type);

            SQ[i].vel = E.lane[i].BV;
            sendNote(0xB0, 15, i + 81, E.lane[i].BV);
            SQ[i].velh = E.lane[i].VA;
            sendNote(0xB0, 15, i + 91, E.lane[i].VA);

            SQ[i].ch = E.lane[i].ch;
            sendNote(0xB0, 15, (i * 10) + 8, E.lane[i].ch);

            SQ[i].gate = E.lane[i].gate;
            if (i != 0)
                sendNote(0xB0, 15, (i * 10) + 7, E.lane[i].gate);
            else
                sendNote(0xB0, 15, (i * 10) + 9, E.lane[i].gate);
            SQ[i].loop = E.lane[i].loop;
            sendNote(0xB0, 15, i + 101, E.lane[i].loop);
            SQ[i].ENABLE(E.lane[i].enabled);
            sendNote(0xB0, 15, (i * 10) + 1, E.lane[i].enabled ? 127 : 0);

            if (!started)
            {

                SQ[i].steps = E.lane[i].steps;
                if (i != 6)
                    sendNote(0xB0, 15, (i * 10) + 4, E.lane[i].steps);
                else
                    sendNote(0xB0, 15, (i * 10) + 9, E.lane[i].steps);

                SQ[i].pulses = E.lane[i].pulses;
                sendNote(0xB0, 15, (i * 10) + 5, E.lane[i].pulses);

                SQ[i].shift = E.lane[i].shift;
                sendNote(0xB0, 15, (i * 10) + 6, E.lane[i].shift);

                SQ[i].updateDiv(E.lane[i].div);

                sendNote(0xB0, 15, (i * 10) + 3, E.lane[i].div);

                SQ[i].note = E.lane[i].note;
                sendNote(0xB0, 15, (i * 10) + 2, E.lane[i].note);

                SQ[i].updateSeq();
            }
            else
            {
                if (i != 6)
                    // sendNote(0xB0, 15, (i * 10) + 4, E.lane[i].steps);
                    queCC((i * 10) + 4, i, E.lane[i].steps, STEP);
                else
                    queCC((i * 10) + 9, i, E.lane[i].steps, STEP);

                //  sendNote(0xB0, 15, (i * 10) + 5, E.lane[i].pulses);
                queCC((i * 10) + 5, i, E.lane[i].pulses, PULSE);

                // sendNote(0xB0, 15, (i * 10) + 6, E.lane[i].shift);
                queCC((i * 10) + 6, i, E.lane[i].shift, SHIFT);
                //  sendNote(0xB0, 15, (i * 10) + 2, E.lane[i].note);
                queCC((i * 10) + 2, i, E.lane[i].note, NOTE);
                // sendNote(0xB0, 15, (i * 10) + 3, E.lane[i].div);
                queCC((i * 10) + 3, i, E.lane[i].div, DIV);

                //   SQ[i].ENABLE(E.lane[i].enabled);
                //  sendNote(0xB0, 15, (i * 10) + 1, E.lane[i].enabled ? 127 : 0);
                //  queCC((i * 10) + 1, i, E.lane[i].enabled, ENABLE);
            }
        }
        printAll(true);
        //  resync(true, true);

        //  cout << "REad " << (int)E.lane[0].pulses << endl;
    }
}
void savePatch(int slot)
{
    if (saving)
        return;
    saving = true;
    EUPATCH E = seqPatch();

    FILE *o;
    o = fopen(basePath.c_str(), "rb+");
    if (o != NULL)
    {
        fseek(o, sizeof(struct EUPATCH) * slot, SEEK_SET);
        fwrite(&E, sizeof(struct EUPATCH), 1, o);
        fclose(o);
    }
    loaded.patch = E;
    loaded.slot = slot;
    if (!bgprocess)
        cout << "Patch Saved to Slot:" << (int)currSlot << endl;
    printAll();
    saving = false;
}
string typeLabel(int trk)
{
    switch (limit(SQ[trk].mode, 1, 4))
    {
    case 0:
    case 1:
        return "NT ";
    case 2:
        return "DR ";
    case 3:
        return "C1 ";
    case 4:
        return "C2 ";
    }
    return "   ";
}

bool needsSync(syncTypes T)
{
    switch (T)
    {
    case ENABLE:
        return false;
        break;
    case NOTE:
        return false;
        break;
    }
    return true;
}
void setSleep()
{
    if (started)
    {
        SLEEP_UNIT = 100;
        return;
    }
    SLEEP_UNIT = 5000;
}

int getRandValue(int lo, int hi)
{
    return rand() % (hi - lo) + lo;
}

void RandomizeLane(int lane)
{
    int i = lane;
    loading = getUS() + (1000 * 100); // 100ms
                                      // cout << "loading is" << loading << endl;
    int rnd = getRandValue(40, 100);
    /* SQ[i].vel = rnd;
     sendNote(0xB0, 15, i + 81, rnd);

     rnd = getRandValue(40, 100);
     SQ[i].velh = rnd;
     sendNote(0xB0, 15, i + 91, rnd);*/
    rnd = getRandValue(0, 32);
    rnd = rnd < 9 ? 0 : rnd - 8;
    rnd = rnd > 16 ? 16 : rnd;
    SQ[i].loop = rnd;
    sendNote(0xB0, 15, i + 101, rnd);

    if (!started)
    {
        rnd = getRandValue(4, 16);
        SQ[i].steps = rnd;
        if (i != 6)
            sendNote(0xB0, 15, (i * 10) + 4, rnd);
        else
            sendNote(0xB0, 15, (i * 10) + 9, rnd);
        cout << RANDMAXFILL << endl;
        cout << SQ[i].steps * RANDMAXFILL << endl;
        float rndMax = (SQ[i].steps * RANDMAXFILL) / 100;
        cout << rndMax << endl;
        int xrnd = (int)rndMax;
        rnd = getRandValue(1, xrnd <= 1 ? 2 : xrnd);
        cout << "rnd is " << rnd << endl;
        SQ[i].pulses = rnd;
        sendNote(0xB0, 15, (i * 10) + 5, rnd);
        rnd = getRandValue(0, 16);
        SQ[i].shift = rnd;
        sendNote(0xB0, 15, (i * 10) + 6, rnd);
        SQ[i].updateSeq();
    }
    else
    {
        rnd = getRandValue(4, 16);
        if (i != 6)
        {
            sendNote(0xB0, 15, (i * 10) + 4, rnd);

            queCC((i * 10) + 4, i, rnd, STEP);
        }
        else
        {
            sendNote(0xB0, 15, (i * 10) + 9, rnd);
            queCC((i * 10) + 9, i, rnd, STEP);
        }

        int rndMax = (int)(SQ[i].steps * RANDMAXFILL) / 100;
        rnd = getRandValue(1, rndMax <= 1 ? 2 : rndMax);
        sendNote(0xB0, 15, (i * 10) + 5, rnd);
        queCC((i * 10) + 5, i, rnd, PULSE);

        rnd = getRandValue(0, 16);
        sendNote(0xB0, 15, (i * 10) + 6, rnd);
        queCC((i * 10) + 6, i, rnd, SHIFT);
    }

    //  resync(true, true);

    //  cout << "REad " << (int)E.lane[0].pulses << endl;
}

/* ------------------------------------------------------------------------
 * Control socket (force-shadow GUI). One request per connection, same text
 * protocol as force-acid/force-maze:  "SET <key> <value>\n" -> "OK\n"|"ERR\n",
 * "GET <key>\n" -> "<value>\n".  Every SET is turned into the equivalent
 * MIDI CC and run through handleMidi(), so socket and MIDI can never diverge.
 * Keys: l<1-8>_<param> or sel_<param> (the GUI-selected lane):
 *   enable note div steps fill shift gate ch vel velh loop mode(0=note,1=drum) toggle(idx, SET only)
 *   GET only: note_txt div_txt info pattern
 * Globals: sel preset preset_load preset_save rand_lane rand_go rand_l<1-8>
 *   GET only: sel_name preset_txt preset_list rand_lane_txt transport
 * `l<N>_toggle`/`sel_toggle` (SET, value = step index) and `rand_l<N>` (multi-select
 * randomize lane picker) have no MIDI CC equivalent -- GUI-only, socket-only.
 * Not exposed: CC track modes, internal clock (dormant, MIDI-only).
 * --------------------------------------------------------------------- */
int selLane = 0;
static bool randMask[8] = {false, false, false, false, false, false, false, false};
// One page of the PRESETS list widget (cols=8 rows=7, see scripts/gen_shadow_page.py); the bank
// still has 128 slots, load/save by number still reach all of them, only the browsable list is capped.
static const int PRESET_LIST_MAX = 56;
static const char *DIV_TXT[] = {"1/16", "1/16", "1/8", "1/4", "1/2", "1", "1/32", "1/24", "1/12", "1/6", "1/3"};
static const char *RAND_TXT[] = {"ALL", "LANE 1", "LANE 2", "LANE 3", "LANE 4", "LANE 5", "LANE 6", "LANE 7", "LANE 8", "ALL BUT 1", "ALL BUT 5"};

static void ctrlMidi(unsigned char cc, unsigned char val)
{
    std::vector<unsigned char> m = {0xB0, cc, val};
    handleMidi(&m);
}
static int laneCC(int i, const string &p) // CC number for a lane parameter, -1 = unknown
{
    if (p == "enable") return i * 10 + 1;
    if (p == "note") return i * 10 + 2;
    if (p == "div") return i * 10 + 3;
    if (p == "steps") return i == 6 ? 69 : i * 10 + 4;
    if (p == "fill") return i * 10 + 5;
    if (p == "shift") return i * 10 + 6;
    if (p == "gate") return i == 0 ? 9 : i * 10 + 7;
    if (p == "ch") return i * 10 + 8;
    if (p == "vel") return 81 + i;
    if (p == "velh") return 91 + i;
    if (p == "loop") return 101 + i;
    if (p == "mode") return 111 + i;
    return -1;
}
static string noteName(int n)
{
    static const char *N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return string(N[n % 12]) + std::to_string(n / 12 - 2);
}
static bool splitKey(const string &key, int &lane, string &param)
{
    if (key.rfind("sel_", 0) == 0)
    {
        lane = selLane;
        param = key.substr(4);
        return true;
    }
    if (key.size() > 3 && key[0] == 'l' && key[1] >= '1' && key[1] <= '8' && key[2] == '_')
    {
        lane = key[1] - '1';
        param = key.substr(3);
        return true;
    }
    return false;
}
static string ctrlGet(const string &key)
{
    if (key == "sel") return std::to_string(selLane);
    if (key == "sel_name") return "LANE " + std::to_string(selLane + 1);
    if (key == "preset") return std::to_string(currSlot);
    if (key == "preset_txt") return "SLOT " + std::to_string(currSlot + 1);
    if (key == "rand_lane") return std::to_string(RANDLANE);
    if (key == "rand_lane_txt") return RAND_TXT[limit(RANDLANE, 0, 10)];
    if (key.size() == 7 && key.rfind("rand_l", 0) == 0 && key[6] >= '1' && key[6] <= '8')
        return randMask[key[6] - '1'] ? "1" : "0";
    if (key == "transport")
    {
        char b[32];
        snprintf(b, sizeof(b), started ? "RUN %d" : "STOP", (int)(BPM + 0.5f));
        return b;
    }
    if (key == "preset_list")
    {
        // Names left blank: the GUI's `numbered=1` prefixes each tile with its slot number
        // (see gen_shadow_page.py) -- "SLOT 10" truncated to "SLOT 1" in an 8-col tile at
        // scale 1.5 (10px/char, ~69px budget), making two-digit slots collide visually.
        string j = "[";
        for (int i = 0; i < PRESET_LIST_MAX; i++)
            j += string(i ? "," : "") + "{\"name\":\"\"}";
        return j + "]";
    }
    int i;
    string p;
    if (!splitKey(key, i, p)) return "ERR";
    EQSEQ &q = SQ[i];
    if (p == "enable") return q.enabled ? "1" : "0";
    if (p == "note") return std::to_string(q.note);
    if (p == "div") return std::to_string(q.div);
    if (p == "steps") return std::to_string(q.steps);
    if (p == "fill") return std::to_string(q.pulses);
    if (p == "shift") return std::to_string(q.shift);
    if (p == "gate") return std::to_string(q.gate);
    if (p == "ch") return std::to_string(q.ch);
    if (p == "vel") return std::to_string(q.vel);
    if (p == "velh") return std::to_string(q.velh);
    if (p == "loop") return std::to_string(q.loop);
    if (p == "mode") return q.mode == 2 ? "1" : "0";
    if (p == "note_txt") return noteName(q.note);
    if (p == "div_txt") return DIV_TXT[limit(q.div, 0, 10)];
    if (p == "info")
    {
        string r = std::to_string(q.steps) + "/" + std::to_string(q.pulses) + " SH" + std::to_string(q.shift);
        if (q.loop > 0) r += " LP" + std::to_string(q.loop);
        return r;
    }
    if (p == "pattern") // steps|bits|play|loop|enabled|selected
    {
        string r = std::to_string(q.steps) + "|";
        const vector<int> &sq = q.pattern();
        for (size_t k = 0; k < sq.size(); k++)
            r += string(k ? "," : "") + (sq[k] ? "1" : "0");
        return r + "|" + std::to_string(q.playStep()) + "|" + std::to_string(q.loop) + "|" + (q.enabled ? "1" : "0") + "|" + (i == selLane ? "1" : "0");
    }
    return "ERR";
}
static bool ctrlSet(const string &key, int v)
{
    if (key == "sel") { selLane = limit(v, 0, 7); return true; }
    if (key == "preset") { ctrlMidi(20, limit(v, 0, 127)); return true; }
    if (key == "preset_load") { ctrlMidi(29, 127); return true; }
    if (key == "preset_save") { ctrlMidi(30, 127); return true; }
    if (key == "rand_lane") { ctrlMidi(89, limit(v, 0, 10)); return true; }
    if (key.size() == 7 && key.rfind("rand_l", 0) == 0 && key[6] >= '1' && key[6] <= '8')
    {
        randMask[key[6] - '1'] = v != 0;
        return true;
    }
    if (key == "rand_go")
    {
        loading = getUS() + (1000 * 100);
        bool any = false;
        for (int i = 0; i < 8; i++)
            if (randMask[i]) { RandomizeLane(i); any = true; }
        if (!any)
            for (int i = 0; i < 8; i++)
                RandomizeLane(i); // nothing picked -> randomize all, same default as before
        printAll(true);
        return true;
    }
    int i;
    string p;
    if (!splitKey(key, i, p)) return false;
    if (p == "toggle") { SQ[i].toggleStep(v); return true; }
    int cc = laneCC(i, p);
    if (cc < 0) return false;
    if (p == "enable") v = v ? 127 : 0;
    if (p == "mode") v = v ? 2 : 1; // NOTE/DRUM only from the GUI
    ctrlMidi((unsigned char)cc, (unsigned char)limit(v, 0, 127));
    return true;
}
static void ctrlThread(int lfd)
{
    for (;;)
    {
        int c = accept(lfd, NULL, NULL);
        if (c < 0)
            continue;
        struct timeval tv = {0, 200 * 1000};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[256];
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        string reply = "ERR";
        if (n > 0)
        {
            buf[n] = 0;
            char *nl = strchr(buf, '\n');
            if (nl) *nl = 0;
            char cmd[8] = {0}, key[64] = {0}, val[64] = {0};
            int f = sscanf(buf, "%7s %63s %63s", cmd, key, val);
            std::lock_guard<std::mutex> lock(engineMutex);
            if (f >= 2 && string(cmd) == "GET")
                reply = ctrlGet(key);
            else if (f >= 3 && string(cmd) == "SET" && ctrlSet(key, (int)atof(val)))
                reply = "OK";
            engineWake = true;
            engineCv.notify_one();
        }
        reply += "\n";
        send(c, reply.c_str(), reply.size(), MSG_NOSIGNAL);
        close(c);
    }
}
void startCtrlSocket()
{
    unlink(ctrlSockPath.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, ctrlSockPath.c_str(), sizeof(a.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 8) != 0)
    {
        close(fd);
        return;
    }
    chmod(ctrlSockPath.c_str(), 0666);
    std::thread(ctrlThread, fd).detach();
}
