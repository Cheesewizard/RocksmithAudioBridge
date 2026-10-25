#include "autogen_airwin/Baxandall2.h"
int Baxandall2_unused = AirwinRegistry::registerAirwindow({"Baxandall2", "Filter", 264, true, "Baxandall2 is Baxandall, extended and made more powerful.", airwinconsolidated::Baxandall2::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::Baxandall2::Baxandall2>(0); }, -1, { }});
#include "autogen_airwin/Cabs.h"
int Cabs_unused = AirwinRegistry::registerAirwindow({"Cabs", "Amp Sims", 40, true, "Cabs is an Airwindows guitar speaker cabinet simulator.", airwinconsolidated::Cabs::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::Cabs::Cabs>(0); }, -1, { }});
#include "autogen_airwin/Chamber2.h"
int Chamber2_unused = AirwinRegistry::registerAirwindow({"Chamber2", "Reverb", 358, false, "Chamber2 is a feedforward reverb, a blur delay, and a glitch topping!", airwinconsolidated::Chamber2::kNumParameters, "2023-02-04", []() { return std::make_unique<airwinconsolidated::Chamber2::Chamber2>(0); }, -1, { "Latest" }});
#include "autogen_airwin/Chorus.h"
int Chorus_unused = AirwinRegistry::registerAirwindow({"Chorus", "Ambience", 23, true, "Chorus is a mono chorus, also works as a vibrato.", airwinconsolidated::Chorus::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::Chorus::Chorus>(0); }, -1, { "Latest" }});
#include "autogen_airwin/Density2.h"
int Density2_unused = AirwinRegistry::registerAirwindow({"Density2", "Distortion", 149, true, "Density2 is a different color for Density, some old code I had that people wanted.", airwinconsolidated::Density2::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::Density2::Density2>(0); }, -1, { }});
#include "autogen_airwin/Drive.h"
int Drive_unused = AirwinRegistry::registerAirwindow({"Drive", "Distortion", 152, true, "Drive is the angry distortion!", airwinconsolidated::Drive::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::Drive::Drive>(0); }, -1, { "Latest" }});
#include "autogen_airwin/Gatelope.h"
int Gatelope_unused = AirwinRegistry::registerAirwindow({"Gatelope", "Dynamics", 207, true, "Gatelope is a special gate that applies filters.", airwinconsolidated::Gatelope::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::Gatelope::Gatelope>(0); }, -1, { "Recommended", "Latest" }});
#include "autogen_airwin/GrindAmp.h"
int GrindAmp_unused = AirwinRegistry::registerAirwindow({"GrindAmp", "Amp Sims", 32, true, "GrindAmp is a heavier amp sim for many purposes.", airwinconsolidated::GrindAmp::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::GrindAmp::GrindAmp>(0); }, -1, { "Latest" }});
#include "autogen_airwin/LeadAmp.h"
int LeadAmp_unused = AirwinRegistry::registerAirwindow({"LeadAmp", "Amp Sims", 34, true, "LeadAmp is an amp sim with a clear, penetrating, vocal tone.", airwinconsolidated::LeadAmp::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::LeadAmp::LeadAmp>(0); }, -1, { "Latest" }});
#include "autogen_airwin/Pressure5.h"
int Pressure5_unused = AirwinRegistry::registerAirwindow({"Pressure5", "Dynamics", 187, false, "Pressure5 expands Pressure4 with after-boost and built-in ClipOnly2.", airwinconsolidated::Pressure5::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::Pressure5::Pressure5>(0); }, -1, { }});
#include "autogen_airwin/TapeDelay2.h"
int TapeDelay2_unused = AirwinRegistry::registerAirwindow({"TapeDelay2", "Ambience", 2, true, "TapeDelay2 is a new implementation of a flexible tape echo.", airwinconsolidated::TapeDelay2::kNumParameters, "2022-11-21", []() { return std::make_unique<airwinconsolidated::TapeDelay2::TapeDelay2>(0); }, -1, { "Recommended", "Latest" }});
#include "autogen_airwin/Verbity2.h"
int Verbity2_unused = AirwinRegistry::registerAirwindow({"Verbity2", "Reverb", 353, false, "Verbity2 adds stereo crossmodulation and expands Verbity's feedforward reverb topology.", airwinconsolidated::Verbity2::kNumParameters, "2023-02-05", []() { return std::make_unique<airwinconsolidated::Verbity2::Verbity2>(0); }, -1, { "Latest" }});
int unusedComplete = AirwinRegistry::completeRegistry();
