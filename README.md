# 20 Questions - M5Cardputer

<p align="center">
  <img src="https://github.com/bpivk/20questions/blob/c52f8e4e211cc05d1164a3124ea8edd222263533/image/cover_320x240.png" alt="20 Questions for M5Cardputer">
</p>

A self-learning 20 Questions game for the [M5Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3) (ESP32-S3). Think of a word, answer 20 yes/no questions, and the handheld tries to guess it. It learns from every game.

## Features

- **1,126 words** out of the box - animals, plants, objects, vehicles, instruments, concepts, fantasy creatures, and more
- **88 questions** covering category, size, material, biology, location, and function
- **Self-learning** - weights update after every game
- **Category picker** - choose Animal, Plant, Object, Concept, or Not Sure
- **Smart question exclusion** - redundant questions are skipped (e.g. "Does it have fur?" after selecting Plant)
- **Personality quips** - thinking comments between questions, with early/mid/late phases
- **Teach new words** - when the game loses, add your word to its vocabulary
- **Statistics** - win/loss ratio, streaks, total questions, last word taught
- **Settings** - sound, screen timeout, learning rate, confirm-on-quit
- **Unlimited words** - streamed from SD in chunks, no PSRAM needed
- **Boot animation** - animated 20Q logo on startup
- **Corruption detection** - checks weight file integrity on boot

## Hardware

- M5Cardputer (M5StampS3 + keyboard + display + SD card slot)
- MicroSD card (any size - the game uses under 200KB)

## Installation

### Option A: WiFi download (easiest)
 
1. Flash `TwentyQ.ino` to the M5Cardputer via Arduino IDE
2. Insert a blank MicroSD card
3. Power on - the game detects missing files and offers to download them via WiFi
4. Pick your WiFi network from the scan list, type the password
5. Files download with progress bars, then the game starts
 
### Option B: Manual file copy
 
1. Flash `TwentyQ.ino` to the M5Cardputer
2. Copy the `sd_card/TwentyQ/` folder to your MicroSD card root:
 
```
SD card root/
  TwentyQ/
    words.csv       (11 KB)
    questions.csv   (2.5 KB)
    weights.bin     (99,100 bytes)
```
 
3. Insert the SD card and power on
 
**Required library:** `M5Cardputer` ([M5Stack GitHub](https://github.com/m5stack/M5Cardputer))
 
> **Note:** If copying manually, verify `weights.bin` is exactly **99,100 bytes** on the SD card.

### 3. Boot and play

Insert the SD card and power on. The boot animation plays, then the word/question count is shown. If the weights file has a problem, a red warning appears.

## How to Play

1. **Think of a word** - anything from "dog" to "lightning" to "pizza"
2. **Pick a category** - Animal, Plant, Object, Concept, or Not Sure
3. **Answer 20 questions** using the answer grid:
   - **Page 1:** Yes, Probably, Usually, No, Unknown, Sometimes
   - **Page 2:** Maybe, Partly, Depends, Rarely, Doubtful, Irrelevant
4. **See the guess** - top 3 guesses with confidence scores
5. **Teach it** - if wrong, type the correct word

### Controls

| Key | Action |
|-----|--------|
| `;` | Up |
| `.` | Down |
| `,` | Left |
| `/` | Right |
| Enter | Select |
| Backspace | Back / Quit |

## How It Works

### Weight Matrix

`weights.bin` stores a `words x questions` matrix of `int8_t` values (-127 to +127). Layout:

```
[4 bytes magic][4 bytes wordCnt][4 bytes qCnt]
[wordCnt x qCnt x int8_t, row-major]
```

### Question Selection

Picks the unasked question with the highest weight variance among plausible words. When weights are cold (few games played), falls back to a split-quality heuristic that prefers ~50/50 divisions.

### Learning

- **Correct guess:** correct word pulled toward answers, wrong words get small decay
- **Wrong guess:** each rejected candidate pushed away from answers
- **Teach:** existing words get strong update; new words appended with game answers as initial weights

## Files

| File | Description |
|------|-------------|
| `TwentyQ.ino` | Main sketch - flash to M5Cardputer |
| `words.csv` | Word list, one per line. `#` = comment, `_` = space |
| `questions.csv` | Question list, one per line. Max 90, 55 chars each |
| `weights.bin` | Pre-built weight matrix (99,100 bytes for 1,126 x 88) |
| `settings.cfg` | Key-value settings, auto-created if missing |
| `stats.cfg` | Game statistics, auto-created if missing |
| `TwentyQ` | Ready-to-copy SD card contents |

## Customization

### Adding words

- **In-game:** lose, then type the word when prompted
- **Manually:** add to `words.csv` - the game auto-extends `weights.bin` on next boot

### Adding questions

Add to `questions.csv`. This triggers a weight reset on next boot (question count mismatch).

### Learning rate

Settings > Learning:
- **Conservative** (0.10) - slow, stable
- **Normal** (0.20) - default
- **Aggressive** (0.35) - fast, may oscillate

### Re-downloading files
 
Settings > Download files will re-download all game files from GitHub via WiFi. This overwrites words, questions, and weights with the latest versions from the repository.

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Red "FILE CORRUPT" on boot | `weights.bin` truncated during copy | Re-copy, verify 99,100 bytes |
| Wrong/random guesses | Weights wiped to zeros | Replace `weights.bin` + `words.csv` |
| Square symbols in quips | Non-ASCII characters in strings | Update to latest sketch |
| Weights reset after teaching | Old sketch uses `FILE_WRITE` (truncates) | Flash latest sketch (uses `"r+"`) |
