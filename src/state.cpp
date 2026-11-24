#include "state.h"
#include "config.h"

std::vector<String> tracks;
size_t currentTrack = 0;
std::vector<size_t> shuffleOrder;
RepeatMode repeatMode = REPEAT_OFF;
bool shuffleEnabled = false;
String playlistDirectory = "";
volatile bool playbackPaused = false;
volatile bool playbackStopped = false;
float currentGain = DEFAULT_GAIN;
volatile uint32_t currentPositionSec = 0;
volatile uint32_t currentTrackDurationSec = 1;
String currentTrackTitle;
String currentTrackArtist;
uint16_t currentTrackBitrate = 0;

String currentDirectory = "/";
std::vector<DirectoryEntry> directoryEntries;
size_t browserSelection = 0;
size_t browserOffset = 0;

Screen currentScreen = SCREEN_BROWSER;
volatile bool uiReady = false;
volatile bool sdMissing = false;
volatile uint8_t splashProgress = 0;
volatile bool otaInProgress = false;
volatile uint8_t otaProgress = 0;
float batteryVoltage = 4.2f;

bool wifiApEnabled = false;
String configuredSsid;
String configuredPassword;

SemaphoreHandle_t audioMutex;
SemaphoreHandle_t stateMutex;
SemaphoreHandle_t sdMutex;
