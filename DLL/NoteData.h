#pragma once

#include <cstdint>

#pragma pack(push, 1)
struct LearnASongNoteData  {
private:
    char padding0[0x30];

    int32_t totalNotesHit;      // 0x30
    int32_t currentHitStreak;   // 0x34
    char padding1[0x04];     // 0x38-0x3B
    int32_t highestHitStreak;   // 0x3C
    int32_t totalNotesMissed;   // 0x40
    int32_t currentMissStreak;  // 0x44

public:
    int32_t getTotalNotesHit() const {
        return totalNotesHit;
    }

    int32_t getTotalNotesMissed() const {
        return totalNotesMissed;
    }

    int32_t getCurrentHitStreak() const {
        return currentHitStreak;
    }

    int32_t getCurrentMissStreak() const {
        return currentMissStreak;
    }

    float getAccuracy() const  {
        if (getTotalNotes() > 0) {
            if (totalNotesHit == 0) {
                return 0.0f;
            }
            return (static_cast<float>(totalNotesHit) / static_cast<float>(getTotalNotes())) * 100.0f;
        }

        return 100.0f;
    }

    int32_t getTotalNotes() const {
        return totalNotesMissed + totalNotesHit;
    }
};
#pragma pack(pop)

#pragma pack(push, 1)
struct ScoreAttackNoteData {
private:
    char padding0[0x3C];

    int32_t currentHitStreak;           // 0x3C
    int32_t currentMissStreak;          // 0x40
    int32_t highestHitStreak;           // 0x44
    int32_t highestMissStreak;          // 0x48
    int32_t totalNotesHit;              // 0x4C
    int32_t totalNotesMissed;           // 0x50
    char padding1[0x20];             // 0x54-0x73
    int32_t currentPerfectHitStreak;    // 0x74
    int32_t totalPerfectHits;           // 0x78
    int32_t currentLateHitStreak;       // 0x7C
    int32_t totalLateHits;              // 0x80
    int32_t perfectPhrases;             // 0x84
    int32_t goodPhrases;                // 0x88
    int32_t passedPhrases;              // 0x8C
    int32_t failedPhrases;              // 0x90
    int32_t currentPerfectPhraseStreak; // 0x94
    int32_t currentGoodPhraseStreak;    // 0x98
    int32_t currentPassedPhraseStreak;  // 0x9C
    int32_t currentFailedPhraseStreak;  // 0xA0
    int32_t highestPerfectPhraseStreak; // 0xA4
    int32_t highestGoodPhraseStreak;    // 0xA8
    int32_t highestPassedPhraseStreak;  // 0xAC
    int32_t highestFailedPhraseStreak;  // 0xB0
    char padding2[0x30];             // 0xB4-0xE3
    int32_t currentScore;               // 0xE4
    int32_t currentMultiplier;          // 0xE8
    int32_t highestMultiplier;          // 0xEC

public:
    float getAccuracy() const  {
        if (getTotalNotes() > 0) {
            if (totalNotesHit == 0) {
                return 0.0f;
            }
            return (static_cast<float>(totalNotesHit) / static_cast<float>(getTotalNotes())) * 100.0f;
        }

        return 100.0f;
    }

    int32_t getTotalNotes() const {
        return totalNotesMissed + totalNotesHit;
    }
};
#pragma pack(pop)
