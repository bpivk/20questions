/*
 * Twenty Questions for M5Cardputer
 *
 * SD card layout:
 *   /TwentyQ/words.csv      - word names, one per line (unlimited size)
 *   /TwentyQ/questions.csv  - questions, one per line (max 90)
 *   /TwentyQ/weights.bin    - weight matrix, auto-created/updated
 *   /TwentyQ/settings.cfg   - sound + timeout prefs
 *
 * Architecture:
 *
 *   weights.bin layout:
 *     [4 bytes magic][4 bytes wordCnt][4 bytes qCnt]
 *     [wordCnt x qCnt x int8_t weights, row-major]
 *
 *   Reads in CHUNK_SIZE chunks, scoring against answers and keeping top-3.
 *
 *   Question selection uses variance-maximising, also chunked.
 *
 *   Learning: in-place weight updates on SD. New words appended.
 *
 * RAM: ~52KB constant (chunk buffer + questions + answers)
 *
 * Controls:
 *   ;  = Up      .  = Down     ,  = Left    /  = Right
 *   Enter = confirm/select      Backspace = back/quit
 */

#include <M5Cardputer.h>
#include <SD.h>
#include <FS.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ── Display ───────────────────────────────────────────────────────────────────
#define DW     240
#define DH     135
#define TITLEH  20
#define BODYY  (TITLEH+2)
#define BODYH  (DH-TITLEH-2)
#define LH      18

// ── Colours ───────────────────────────────────────────────────────────────────
#define C_BG    TFT_BLACK
#define C_FG    TFT_WHITE
#define C_DIM   0x4228u
#define C_SELBG 0x0338u
#define C_TITBG 0x000Bu
#define C_TITFG 0x07FFu
#define C_OK    0x07E0u
#define C_ERR   TFT_RED
#define C_WARN  TFT_YELLOW

// ── Limits ────────────────────────────────────────────────────────────────────
#define MAX_QUESTIONS   90
#define MAX_Q_PER_GAME  20
#define Q_LEN           60
#define WORD_LEN        32
#define CHUNK_SIZE     500    // words processed per SD read - 45KB, ~80ms

// ── Learning ──────────────────────────────────────────────────────────────────
#define LEARN_RATE       0.20f
#define LEARN_RATE_SOFT  0.01f

// ── Answers ───────────────────────────────────────────────────────────────────
// ── Answer values ─────────────────────────────────────────────────────────────
// ANS_IRRELEVANT - excluded from scoring and variance.
#define ANS_YES         (+1.0f)
#define ANS_NO          (-1.0f)
#define ANS_NONE        ( 0.0f)
#define ANS_IRRELEVANT  (-99.0f)

// ── Answer values - two pages, 2 rows x 3 cols ────────────────────────────────
// Page 0 (common):   Yes | Probably | Usually  /  No | Unknown | Sometimes
// Page 1 (nuanced):  Maybe | Partly | Depends  /  Rarely | Doubtful | Irrelevant
// Grid index = page*6 + row*3 + col
struct AnswerDef { const char* label; float value; uint16_t colour; };

static const AnswerDef ANSWERS[12] = {
    // Page 0, row 0
    { "Yes",        +1.00f, 0x07E0u },   // green
    { "Probably",   +0.75f, 0x47E0u },   // lime
    { "Usually",    +0.60f, 0x8FE0u },   // yellow-green
    // Page 0, row 1
    { "No",         -1.00f, 0xF800u },   // red
    { "Unknown",     0.00f, 0x7BCFu },   // light blue
    { "Sometimes",  +0.25f, 0xFFE0u },   // yellow
    // Page 1, row 0
    { "Maybe",       0.00f, 0xFFFFu },   // white
    { "Partly",     +0.10f, 0xFD00u },   // orange
    { "Depends",    +0.10f, 0xFD00u },   // orange-yellow
    // Page 1, row 1
    { "Rarely",     -0.25f, 0xFB60u },   // orange-red
    { "Doubtful",   -0.60f, 0xF940u },   // red-orange
    { "Irrelevant",  ANS_IRRELEVANT, 0x4228u }, // grey
};
#define ANSWER_COUNT 12
#define ANS_COLS  3
#define ANS_ROWS  2
#define ANS_PAGES 2

// ── Keys ──────────────────────────────────────────────────────────────────────
// M5Cardputer physical D-pad layout:
//   Up=';'  Down='.'  Left=','  Right='/'  Enter='\r'  Backspace='\b'
#define KUP    ';'
#define KDOWN  '.'
#define KLEFT  ','
#define KRIGHT '/'

// ── Weight file magic ─────────────────────────────────────────────────────────
#define WEIGHT_MAGIC 0x35515157u   // "20QW5"
#define WEIGHT_HDR   12            // magic(4) + wordCnt(4) + qCnt(4)

// ── Structs ───────────────────────────────────────────────────────────────────
struct LItem { char label[52]; uint16_t lc, dot; };
struct Settings {
    bool soundOn;
    int  timeoutSec;
    bool confirmQuit;      // prompt before quitting a game
    int  learnRate;        // 0=conservative 1=normal 2=aggressive
};
struct Candidate { int idx; float score; };
struct Stats {
    int  playerWins;
    int  computerWins;
    int  gamesPlayed;
    int  currentStreak;    // + = computer streak, - = player streak
    int  bestStreak;       // best computer win streak
    int  bestPlayerStreak; // best player win streak
    int  totalQuestions;   // sum of questions asked across all games
    char lastWord[WORD_LEN]; // last word that stumped it
};
#define TOP_N 3

// Learning rate values indexed by g_cfg.learnRate
static const float LEARN_RATES[]      = { 0.10f, 0.20f, 0.35f };
static const float LEARN_RATES_SOFT[] = { 0.005f, 0.01f, 0.02f };

// ── Globals ───────────────────────────────────────────────────────────────────
static char    g_questions[MAX_QUESTIONS][Q_LEN];
static int     g_qCnt      = 0;
static int     g_wordCnt   = 0;

static float   g_answers[MAX_QUESTIONS];
static int     g_askedOrder[MAX_QUESTIONS];  // all answered questions (category + exclusions + game)
static int     g_askedCnt  = 0;

// Chunk buffer - the only large allocation, lives on heap
// CHUNK_SIZE words x MAX_QUESTIONS questions x 1 byte = 45KB
static int8_t* g_chunk     = nullptr;

static Settings      g_cfg       = { false, 60, true, 1 };
static Stats         g_stats     = { 0, 0, 0, 0, 0, 0, 0, "" };
static unsigned long g_lastInput = 0;

// ── Personality comments - shown between questions to mask SD I/O ─────────────
// Three phases matching the original 20Q toy's personality arc.
// Large pools mean the game rarely repeats the same quip.

static const char* const QUIPS_EARLY[] = {
    "Interesting...",
    "I see...",
    "Hmm, let me think.",
    "Getting warmer.",
    "I have some ideas.",
    "Narrowing it down.",
    "That's a clue!",
    "Oooh, intriguing.",
    "I'm paying attention.",
    "Tell me more.",
    "Every answer helps.",
    "Processing...",
    "My gears are turning.",
    "A clue at last!",
    "I'm taking notes.",
    "Something stirs...",
    "I feel a hunch.",
    "Curious, curious.",
    "Not what I expected.",
    "You're giving me ideas.",
    "I'm warming up.",
    "The plot thickens.",
    "Now we're talking.",
    "I like this puzzle.",
    "Filing that away...",
    "Interesting choice.",
    "I wasn't expecting that.",
    "Let me recalibrate.",
    "My database is stirring.",
    "A new lead!",
};
static const char* const QUIPS_MID[] = {
    "You think you can fool me?",
    "I'm getting closer...",
    "Don't be so sure!",
    "I almost have it.",
    "My circuits are tingling.",
    "I know more than you think.",
    "I'm narrowing it down.",
    "Ha! I have a theory.",
    "You're not as tricky as you think.",
    "I can almost taste it.",
    "Don't give up now.",
    "I'm locking on target.",
    "The clues are adding up.",
    "I have a very strong hunch.",
    "My confidence is rising.",
    "You can't hide from logic.",
    "I'm zeroing in.",
    "Things are becoming clear.",
    "I've seen this pattern before.",
    "My neurons are firing.",
    "Is that your final answer?",
    "I believe I know what it is.",
    "This is all coming together.",
    "I'm closing in on you.",
    "You won't escape my logic.",
    "Are you sure about that answer?",
    "The pieces are fitting.",
    "I think I have you cornered.",
    "You're running out of room!",
    "My prediction matrix is hot.",
    "Don't think - I might feel it.",
    "Halfway there. I'm confident.",
    "I'm building a picture of this.",
    "You'd better be honest with me!",
    "I sense a trick question coming.",
    "Fascinating. Just fascinating.",
    "I'm not confused - just thorough.",
    "My inner voice is whispering...",
    "I have three suspects in mind.",
    "Eliminating possibilities...",
};
static const char* const QUIPS_LATE[] = {
    "I know what it is!",
    "You can't hide from me.",
    "Got you now!",
    "Just confirming...",
    "I'm very confident.",
    "Prepare to be amazed.",
    "I've cracked it!",
    "It's all clear to me now.",
    "The answer is obvious to me.",
    "I'm 99% sure. Watch this.",
    "You played well, but I've got you.",
    "My final computations are done.",
    "I know exactly what you're thinking.",
    "No more questions needed.",
    "Don't even try to bluff me.",
    "This was a good challenge!",
    "I've seen through your tricks.",
    "Almost there - brace yourself.",
    "My logic is airtight.",
    "I have deduced the truth.",
    "You've met your match.",
    "Nothing gets past me.",
    "I could have guessed sooner.",
    "The neural net has spoken.",
    "My certainty is absolute.",
    "Checkmate.",
    "I've enjoyed this. You'll enjoy my answer less.",
    "Final answer loading...",
    "Drumroll, please.",
    "Stand by for genius.",
};
#define QUIPS_EARLY_COUNT 30
#define QUIPS_MID_COUNT   40
#define QUIPS_LATE_COUNT  30

// ── Forward declarations ──────────────────────────────────────────────────────
void runGame();
void runMenu();
void howToPlay();
void settingsMenu();
void statsScreen();
bool loadAll();
bool createWeightFile();
bool resizeWeightFile(int oldWordCnt);
static void pageDots(int cur, int total);

// ═══════════════════════════════════════════════════════════════════════════════
//  SOUND
// ═══════════════════════════════════════════════════════════════════════════════
void beep(int freq=1000, int ms=30){
    if(!g_cfg.soundOn) return;
#ifdef ARDUINO_M5STACK_CARDPUTER
    M5Cardputer.Speaker.tone(freq,ms);
#endif
}

// ═══════════════════════════════════════════════════════════════════════════════
//  TIMEOUT
// ═══════════════════════════════════════════════════════════════════════════════
void touchInput(){ g_lastInput=millis(); M5Cardputer.Display.setBrightness(128); }
void checkTimeout(){
    if(!g_cfg.timeoutSec) return;
    if((millis()-g_lastInput)/1000>=(unsigned long)g_cfg.timeoutSec)
        M5Cardputer.Display.setBrightness(0);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  INPUT
// ═══════════════════════════════════════════════════════════════════════════════
char waitCh(){
    while(true){
        vTaskDelay(20/portTICK_PERIOD_MS); checkTimeout(); M5Cardputer.update();
        if(M5Cardputer.Keyboard.isChange()&&M5Cardputer.Keyboard.isPressed()){
            touchInput(); auto st=M5Cardputer.Keyboard.keysState();
            if(st.enter){beep(1200,25);return '\r';}
            if(st.del)  {beep(600, 25);return '\b';}
            for(auto c:st.word){
                if((uint8_t)c==0x1b){beep(600,25);return '\b';}
                beep(1000,20); return c;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  DISPLAY HELPERS
// ═══════════════════════════════════════════════════════════════════════════════
void titleBar(const char* t, const char* r=nullptr){
    M5Cardputer.Display.fillRect(0,0,DW,TITLEH,C_TITBG);
    M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_TITFG,C_TITBG);
    M5Cardputer.Display.setCursor(4,2); M5Cardputer.Display.print(t);
    if(r){ int rw=strlen(r)*12; M5Cardputer.Display.setTextColor(C_WARN,C_TITBG);
           M5Cardputer.Display.setCursor(DW-rw-4,2); M5Cardputer.Display.print(r); }
}

void screenInit(const char* t){
    M5Cardputer.Display.fillScreen(C_BG); titleBar(t);
    M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_FG,C_BG);
    M5Cardputer.Display.setCursor(0,BODYY);
}
void bprint(const char* s, uint16_t col=C_FG){
    int lim=DH-LH;
    if(M5Cardputer.Display.getCursorY()>lim){
        M5Cardputer.Display.scroll(0,-LH);
        M5Cardputer.Display.fillRect(0,lim,DW,LH,C_BG);
        M5Cardputer.Display.setCursor(0,lim);
    }
    M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(col,C_BG);
    M5Cardputer.Display.println(s);
}
void bprintf(uint16_t col, const char* fmt, ...){
    char buf[128]; va_list a; va_start(a,fmt); vsnprintf(buf,128,fmt,a); va_end(a);
    bprint(buf,col);
}
int visRows(){ return BODYH/LH; }

// ═══════════════════════════════════════════════════════════════════════════════
//  PERSONALITY QUIPS
// ═══════════════════════════════════════════════════════════════════════════════
// Call this BEFORE the SD read you want to mask.
// It prints a quip and animates dots while the caller does its work.
// Returns immediately after printing the quip - the dots animate via
// showThinkingDots() which the caller can call once loading is done.

void showQuip(int questionNum){
    const char* quip;
    if(questionNum <= 7)       quip = QUIPS_EARLY[random(QUIPS_EARLY_COUNT)];
    else if(questionNum <= 15) quip = QUIPS_MID  [random(QUIPS_MID_COUNT)];
    else                       quip = QUIPS_LATE [random(QUIPS_LATE_COUNT)];

    M5Cardputer.Display.fillScreen(C_BG);
    titleBar("Thinking...");
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(C_TITFG, C_BG);
    M5Cardputer.Display.setCursor(4, BODYY + 4);
    M5Cardputer.Display.print(quip);
}

// Called after the SD load that follows showQuip().
// Shows a visible blinking prompt bar and waits for keypress.
void waitForQuipDismiss(){
    // Draw the prompt bar at the bottom
    const int barY = DH - 14;
    const int barH = 12;
    bool visible = true;
    unsigned long start = millis();
    unsigned long lastBlink = start;

    // Initial draw
    M5Cardputer.Display.fillRect(0, barY, DW, barH, C_SELBG);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(C_FG, C_SELBG);
    int tw = 17 * 6;  // "press any key..." width at size 1
    M5Cardputer.Display.setCursor((DW - tw) / 2, barY + 2);
    M5Cardputer.Display.print("press any key...");

    while(true){
        vTaskDelay(20 / portTICK_PERIOD_MS);
        checkTimeout();
        M5Cardputer.update();

        // Auto-dismiss after 4 seconds
        if(millis() - start > 4000) return;

        // Blink the prompt bar every 600ms
        if(millis() - lastBlink > 600){
            lastBlink = millis();
            visible = !visible;
            if(visible){
                M5Cardputer.Display.fillRect(0, barY, DW, barH, C_SELBG);
                M5Cardputer.Display.setTextSize(1);
                M5Cardputer.Display.setTextColor(C_FG, C_SELBG);
                M5Cardputer.Display.setCursor((DW - tw) / 2, barY + 2);
                M5Cardputer.Display.print("press any key...");
            } else {
                M5Cardputer.Display.fillRect(0, barY, DW, barH, C_BG);
            }
        }

        // Check for keypress
        if(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()){
            touchInput();
            beep(1000, 20);
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WORD WRAP
// ═══════════════════════════════════════════════════════════════════════════════
static int wrapText(const char* text, char lines[][32], int maxLines, int maxChars){
    int n=0,len=strlen(text),pos=0;
    while(pos<len&&n<maxLines){
        int end=pos+maxChars;
        if(end>=len){end=len;}
        else{int b=end;while(b>pos&&text[b]!=' ')b--;if(b>pos)end=b;}
        int l=end-pos; if(l>31)l=31;
        strncpy(lines[n],text+pos,l); lines[n][l]='\0'; n++;
        pos=end; while(pos<len&&text[pos]==' ')pos++;
    }
    return n;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  MULTI-ANSWER QUESTION SCREEN  -  2-page 2×3 grid
//
//  Page 0 (common):   Yes      | Probably | Usually
//                     No       | Unknown  | Sometimes
//  Page 1 (nuanced):  Maybe    | Partly   | Depends
//                     Rarely   | Doubtful | Irrelevant
//
//  Navigation: arrows within page; Down on bottom row → next page;
//              Up on top row → prev page; Enter = confirm; Bksp = quit
// ═══════════════════════════════════════════════════════════════════════════════
float askQuestion(const char* question, int qNum, int qTotal, bool* quit){
    *quit = false;

    const int COLS   = ANS_COLS;    // 3
    const int ROWS   = ANS_ROWS;    // 2
    const int CELL_W = DW / COLS;   // 80

    const int QCHARS = 19, QMAXLINES = 2;
    char qLines[QMAXLINES][32];
    int  nQL     = wrapText(question, qLines, QMAXLINES, QCHARS);
    int  gridTop = BODYY + nQL * LH + 6;
    int  gridH   = DH - gridTop - 2;
    int  CELL_H  = gridH / ROWS;

    int page = 0, row = 0, col = 0;
    char qBuf[14]; snprintf(qBuf, 14, "Q%d/%d", qNum, qTotal);

    auto ansIdx = [&](){ return page * COLS * ROWS + row * COLS + col; };

    auto drawCell = [&](int r, int c, int pg, bool hi){
        int ai      = pg * COLS * ROWS + r * COLS + c;
        int x       = c * CELL_W;
        int y       = gridTop + r * CELL_H;
        uint16_t ac = ANSWERS[ai].colour;
        uint16_t bg = hi ? ac  : C_BG;
        uint16_t fg = hi ? C_BG : ac;
        M5Cardputer.Display.fillRect(x, y, CELL_W-1, CELL_H-1, bg);
        M5Cardputer.Display.drawRect(x, y, CELL_W-1, CELL_H-1, hi ? C_FG : C_DIM);
        int lw = (int)strlen(ANSWERS[ai].label) * 6;
        int lx = x + (CELL_W - 1 - lw) / 2;
        int ly = y + (CELL_H - 1 - 8)  / 2;
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setTextColor(fg, bg);
        M5Cardputer.Display.setCursor(lx, ly);
        M5Cardputer.Display.print(ANSWERS[ai].label);
    };

    auto draw = [&](){
        M5Cardputer.Display.fillScreen(C_BG);
        titleBar(qBuf);
        M5Cardputer.Display.setTextSize(2);
        for(int i = 0; i < nQL; i++){
            M5Cardputer.Display.setTextColor(C_FG, C_BG);
            M5Cardputer.Display.setCursor(4, BODYY + i * LH);
            M5Cardputer.Display.print(qLines[i]);
        }
        // Page dots in title bar
        M5Cardputer.Display.fillCircle(DW-14, TITLEH/2, 3, page==0 ? C_FG : C_DIM);
        M5Cardputer.Display.fillCircle(DW- 6, TITLEH/2, 3, page==1 ? C_FG : C_DIM);
        for(int r = 0; r < ROWS; r++)
            for(int c = 0; c < COLS; c++)
                drawCell(r, c, page, (r==row && c==col));
    };

    draw();
    while(true){
        char ch = waitCh();
        int pr = row, pc = col, pp = page;

        if(ch == KUP){
            if(row > 0)          row--;
            else if(page > 0){ page--; row = ROWS-1; }   // wrap to prev page bottom
        } else if(ch == KDOWN){
            if(row < ROWS-1)     row++;
            else if(page < ANS_PAGES-1){ page++; row = 0; }  // wrap to next page top
        } else if(ch == KLEFT){
            if(col > 0) col--;
        } else if(ch == KRIGHT){
            if(col < COLS-1) col++;
        } else if(ch == '\r'){
            return ANSWERS[ansIdx()].value;
        } else if(ch == '\b'){
            *quit = true; return ANS_NONE;
        }

        if(row!=pr || col!=pc || page!=pp) draw();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SCROLLABLE MENU LIST
// ═══════════════════════════════════════════════════════════════════════════════
void drawList(const LItem* items,int cnt,int sel,int sc,const char* title){
    M5Cardputer.Display.fillScreen(C_BG); titleBar(title);
    if(!cnt){M5Cardputer.Display.setTextSize(2);M5Cardputer.Display.setTextColor(C_DIM,C_BG);
             M5Cardputer.Display.setCursor(6,BODYY+6);M5Cardputer.Display.print("(empty)");return;}
    int rows=visRows(); M5Cardputer.Display.setTextSize(2);
    for(int i=0;i<rows&&(i+sc)<cnt;i++){
        int idx=i+sc; bool hi=(idx==sel); uint16_t bg=hi?C_SELBG:C_BG;
        int y=BODYY+i*LH;
        if(hi) M5Cardputer.Display.fillRect(0,y,DW,LH,C_SELBG);
        if(items[idx].dot) M5Cardputer.Display.fillCircle(5,y+LH/2,3,items[idx].dot);
        M5Cardputer.Display.setTextColor(hi?(uint16_t)C_FG:items[idx].lc,bg);
        M5Cardputer.Display.setCursor(13,y+1); M5Cardputer.Display.print(items[idx].label);
    }
}
int runList(LItem* items,int cnt,const char* title,int startSel=0){
    int rows=visRows(),sel=(cnt>0&&startSel<cnt)?startSel:0,sc=(sel>=rows)?sel-rows+1:0;
    drawList(items,cnt,sel,sc,title);
    while(true){
        char c=waitCh();
        if(c==KUP&&sel>0)          {sel--;if(sel<sc)sc=sel;drawList(items,cnt,sel,sc,title);}
        else if(c==KDOWN&&sel<cnt-1){sel++;if(sel>=sc+rows)sc=sel-rows+1;drawList(items,cnt,sel,sc,title);}
        else if(c=='\r')            return cnt>0?sel:-1;
        else if(c=='\b')            return -1;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXT INPUT
// ═══════════════════════════════════════════════════════════════════════════════
String typeText(const char* title,const char* prompt,const char* prefill=""){
    M5Cardputer.Display.fillScreen(C_BG); titleBar(title);
    M5Cardputer.Display.setTextSize(1); M5Cardputer.Display.setTextColor(C_DIM,C_BG);
    M5Cardputer.Display.setCursor(4,BODYY+2); M5Cardputer.Display.print(prompt);
    String val=String(prefill); int iy=BODYY+14;
    auto redraw=[&](){
        M5Cardputer.Display.fillRect(0,iy,DW,LH+4,C_BG);
        M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_FG,C_BG);
        M5Cardputer.Display.setCursor(4,iy); M5Cardputer.Display.print(val);
        M5Cardputer.Display.fillRect(M5Cardputer.Display.getCursorX(),iy,8,LH,C_TITFG);
    };
    redraw();
    while(true){
        vTaskDelay(10/portTICK_PERIOD_MS); checkTimeout(); M5Cardputer.update();
        if(!M5Cardputer.Keyboard.isChange()||!M5Cardputer.Keyboard.isPressed()) continue;
        touchInput(); auto st=M5Cardputer.Keyboard.keysState();
        if(st.enter) return val;
        if(st.del){if(val.length()){val.remove(val.length()-1);redraw();}continue;}
        for(auto ch:st.word){if((uint8_t)ch==0x1b)return "";val+=ch;} redraw();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CONFIRM DIALOG
// ═══════════════════════════════════════════════════════════════════════════════
bool confirmDialog(const char* title,const char* q,bool defYes=true){
    int sel=defYes?0:1;
    auto draw=[&](){
        M5Cardputer.Display.fillScreen(C_BG); titleBar(title);
        M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_WARN,C_BG);
        M5Cardputer.Display.setCursor(4,BODYY+2); M5Cardputer.Display.print(q);
        int by=BODYY+LH+16;
        uint16_t yb=sel==0?C_OK:C_DIM, nb=sel==1?C_ERR:C_DIM;
        M5Cardputer.Display.fillRoundRect(18, by,84,LH+4,4,yb);
        M5Cardputer.Display.fillRoundRect(136,by,84,LH+4,4,nb);
        M5Cardputer.Display.setTextColor(C_BG,yb); M5Cardputer.Display.setCursor(44, by+2); M5Cardputer.Display.print("YES");
        M5Cardputer.Display.setTextColor(C_BG,nb); M5Cardputer.Display.setCursor(164,by+2); M5Cardputer.Display.print("NO");
    };
    draw();
    while(true){
        char c=waitCh();
        if(c==KUP||c==KDOWN||c==KLEFT||c==KRIGHT){sel=1-sel;draw();}
        if(c=='\r') return sel==0;
        if(c=='\b') return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SETTINGS
// ═══════════════════════════════════════════════════════════════════════════════
void loadSettings(){
    File f=SD.open("/TwentyQ/settings.cfg"); if(!f)return;
    String s=f.readString(); f.close(); int pos=0;
    while(pos<(int)s.length()){
        int nl=s.indexOf('\n',pos); if(nl<0)nl=s.length();
        String line=s.substring(pos,nl); line.trim(); int eq=line.indexOf('=');
        if(eq>0){
            String k=line.substring(0,eq);k.trim(); String v=line.substring(eq+1);v.trim();
            if(k=="sound")       g_cfg.soundOn    =(v=="1");
            if(k=="timeout")     g_cfg.timeoutSec =v.toInt();
            if(k=="confirmquit") g_cfg.confirmQuit=(v=="1");
            if(k=="learnrate")   g_cfg.learnRate  =constrain(v.toInt(),0,2);
        }
        pos=nl+1;
    }
}
void saveSettings(){
    File f=SD.open("/TwentyQ/settings.cfg",FILE_WRITE); if(!f)return;
    f.printf("sound=%d\ntimeout=%d\nconfirmquit=%d\nlearnrate=%d\n",
             g_cfg.soundOn?1:0, g_cfg.timeoutSec,
             g_cfg.confirmQuit?1:0, g_cfg.learnRate);
    f.close();
}

void loadStats(){
    File f=SD.open("/TwentyQ/stats.cfg"); if(!f)return;
    String s=f.readString(); f.close(); int pos=0;
    while(pos<(int)s.length()){
        int nl=s.indexOf('\n',pos); if(nl<0)nl=s.length();
        String line=s.substring(pos,nl); line.trim(); int eq=line.indexOf('=');
        if(eq>0){
            String k=line.substring(0,eq);k.trim(); String v=line.substring(eq+1);v.trim();
            if(k=="player")      g_stats.playerWins     =v.toInt();
            if(k=="computer")    g_stats.computerWins   =v.toInt();
            if(k=="played")      g_stats.gamesPlayed    =v.toInt();
            if(k=="streak")      g_stats.currentStreak  =v.toInt();
            if(k=="beststreak")  g_stats.bestStreak     =v.toInt();
            if(k=="bestplayer")  g_stats.bestPlayerStreak=v.toInt();
            if(k=="totalq")      g_stats.totalQuestions =v.toInt();
            if(k=="lastword")    strncpy(g_stats.lastWord,v.c_str(),WORD_LEN-1);
        }
        pos=nl+1;
    }
}
void saveStats(){
    File f=SD.open("/TwentyQ/stats.cfg",FILE_WRITE); if(!f)return;
    f.printf("player=%d\ncomputer=%d\nplayed=%d\nstreak=%d\nbeststreak=%d\nbestplayer=%d\ntotalq=%d\nlastword=%s\n",
             g_stats.playerWins, g_stats.computerWins, g_stats.gamesPlayed,
             g_stats.currentStreak, g_stats.bestStreak, g_stats.bestPlayerStreak,
             g_stats.totalQuestions, g_stats.lastWord);
    f.close();
}

// ── Settings menu ─────────────────────────────────────────────────────────────
// Up/Down scrolls through rows (textSize 2, LH rows - matches the list style).
// Left/Right cycles the value of the selected setting.
// Enter executes action rows (Reload, Reset stats, Reset weights).
// Backspace exits.
void settingsMenu(){
    const int NROWS = 7;
    int sel = 0;
    int sc  = 0;   // scroll offset

    struct SettingRow {
        const char* key;
        char        val[28];
        uint16_t    vc;
        bool        isAction;
    };

    auto buildRows = [&](SettingRow rows[NROWS]){
        rows[0].key = "Sound";
        snprintf(rows[0].val, 28, "%s", g_cfg.soundOn ? "ON" : "OFF");
        rows[0].vc = g_cfg.soundOn ? C_OK : C_DIM;
        rows[0].isAction = false;

        const char* ts = g_cfg.timeoutSec==30  ? "30 sec" :
                         g_cfg.timeoutSec==60  ? "1 min"  :
                         g_cfg.timeoutSec==120 ? "2 min"  : "Off";
        rows[1].key = "Timeout";
        snprintf(rows[1].val, 28, "%s", ts);
        rows[1].vc = g_cfg.timeoutSec ? C_TITFG : C_DIM;
        rows[1].isAction = false;

        const char* lr = g_cfg.learnRate==0 ? "Conservative" :
                         g_cfg.learnRate==2 ? "Aggressive"   : "Normal";
        rows[2].key = "Learning";
        snprintf(rows[2].val, 28, "%s", lr);
        rows[2].vc = C_TITFG;
        rows[2].isAction = false;

        rows[3].key = "Confirm quit";
        snprintf(rows[3].val, 28, "%s", g_cfg.confirmQuit ? "Yes" : "No");
        rows[3].vc = g_cfg.confirmQuit ? C_OK : C_DIM;
        rows[3].isAction = false;

        rows[4].key = "Reload wordlist";
        rows[4].val[0] = 0;
        rows[4].vc = C_DIM;
        rows[4].isAction = true;

        rows[5].key = "Reset stats";
        rows[5].val[0] = 0;
        rows[5].vc = C_WARN;
        rows[5].isAction = true;

        rows[6].key = "Reset weights";
        rows[6].val[0] = 0;
        rows[6].vc = C_ERR;
        rows[6].isAction = true;
    };

    auto draw = [&](){
        SettingRow rows[NROWS];
        buildRows(rows);

        M5Cardputer.Display.fillScreen(C_BG);
        titleBar("Settings");

        int visible = visRows();
        for(int i = 0; i < visible && (i + sc) < NROWS; i++){
            int idx = i + sc;
            bool hi = (idx == sel);
            int y   = BODYY + i * LH;
            uint16_t bg = hi ? C_SELBG : C_BG;

            if(hi) M5Cardputer.Display.fillRect(0, y, DW, LH, C_SELBG);

            // Key label - left side, textSize 2
            M5Cardputer.Display.setTextSize(2);
            M5Cardputer.Display.setTextColor(hi ? C_FG : C_DIM, bg);
            M5Cardputer.Display.setCursor(6, y + 1);
            M5Cardputer.Display.print(rows[idx].key);

            if(!rows[idx].isAction && rows[idx].val[0]){
                // Value - right-aligned, coloured
                int vx = DW - (int)strlen(rows[idx].val) * 12 - 6;
                M5Cardputer.Display.setTextColor(hi ? C_FG : rows[idx].vc, bg);
                M5Cardputer.Display.setCursor(vx, y + 1);
                M5Cardputer.Display.print(rows[idx].val);
                if(hi){
                    // < > cycle indicators
                    M5Cardputer.Display.setTextColor(C_DIM, bg);
                    M5Cardputer.Display.setCursor(vx - 14, y + 1);
                    M5Cardputer.Display.print("<");
                    M5Cardputer.Display.setCursor(DW - 14, y + 1);
                    M5Cardputer.Display.print(">");
                }
            } else if(rows[idx].isAction && hi){
                M5Cardputer.Display.setTextColor(rows[idx].vc, bg);
                M5Cardputer.Display.setCursor(DW - 12*7 - 6, y + 1);
                M5Cardputer.Display.print("[Enter]");
            }
        }
    };

    draw();
    while(true){
        char c = waitCh();
        int visible = visRows();

        if(c == KUP && sel > 0){
            sel--;
            if(sel < sc) sc = sel;
            draw();
        } else if(c == KDOWN && sel < NROWS - 1){
            sel++;
            if(sel >= sc + visible) sc = sel - visible + 1;
            draw();
        } else if(c == '\b'){
            return;
        } else if(c == KLEFT || c == KRIGHT){
            int d = (c == KRIGHT) ? 1 : -1;
            if     (sel == 0){ g_cfg.soundOn = !g_cfg.soundOn; saveSettings(); }
            else if(sel == 1){
                const int vals[] = {0, 30, 60, 120};
                int cur = 0;
                for(int i = 0; i < 4; i++) if(vals[i] == g_cfg.timeoutSec) cur = i;
                cur = (cur + 4 + d) % 4;
                g_cfg.timeoutSec = vals[cur];
                saveSettings();
            }
            else if(sel == 2){ g_cfg.learnRate = (g_cfg.learnRate + 3 + d) % 3; saveSettings(); }
            else if(sel == 3){ g_cfg.confirmQuit = !g_cfg.confirmQuit; saveSettings(); }
            draw();
        } else if(c == '\r'){
            if(sel == 4){
                M5Cardputer.Display.fillScreen(C_BG); titleBar("Reloading...");
                M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_DIM, C_BG);
                M5Cardputer.Display.setCursor(4, BODYY + 4);
                M5Cardputer.Display.print("Counting words...");
                loadAll();
                M5Cardputer.Display.setTextColor(C_OK, C_BG);
                M5Cardputer.Display.setCursor(4, BODYY + 4 + LH);
                M5Cardputer.Display.printf("%d words loaded", g_wordCnt);
                delay(1200);
                draw();
            } else if(sel == 5){
                if(confirmDialog("Reset stats?", "Wipe all stats?", false)){
                    g_stats = {0,0,0,0,0,0,0,""};
                    saveStats();
                    M5Cardputer.Display.fillScreen(C_BG); titleBar("Done");
                    M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_OK, C_BG);
                    M5Cardputer.Display.setCursor(4, BODYY + 4);
                    M5Cardputer.Display.print("Stats cleared.");
                    delay(900);
                    draw();
                }
            } else if(sel == 6){
                if(confirmDialog("Reset weights?", "Wipe all learning?", false)){
                    M5Cardputer.Display.fillScreen(C_BG); titleBar("Resetting...");
                    M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_WARN, C_BG);
                    M5Cardputer.Display.setCursor(4, BODYY + 4);
                    M5Cardputer.Display.print("Rebuilding...");
                    M5Cardputer.Display.setTextColor(C_DIM, C_BG);
                    M5Cardputer.Display.setCursor(4, BODYY + 4 + LH);
                    M5Cardputer.Display.print("(may take a moment)");
                    createWeightFile();
                    M5Cardputer.Display.setTextColor(C_OK, C_BG);
                    M5Cardputer.Display.setCursor(4, BODYY + 4 + LH * 2);
                    M5Cardputer.Display.print("Done!");
                    delay(900);
                    draw();
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WEIGHTS.BIN  -  SD-resident, read/written in chunks
// ═══════════════════════════════════════════════════════════════════════════════

// Return file offset of word w's weight row
static inline uint32_t weightOffset(int w){
    return WEIGHT_HDR + (uint32_t)w * g_qCnt;
}

// Write the weights.bin header (called when creating or after wordCnt changes)
bool writeWeightHeader(File& f, int wordCnt, int qCnt){
    f.seek(0);
    uint32_t magic=WEIGHT_MAGIC;
    f.write((uint8_t*)&magic,   4);
    f.write((uint8_t*)&wordCnt, 4);
    f.write((uint8_t*)&qCnt,    4);
    return true;
}

// Create a fresh weights.bin with zeroed weights for all known words
bool createWeightFile(){
    File f=SD.open("/TwentyQ/weights.bin",FILE_WRITE); if(!f)return false;
    writeWeightHeader(f,g_wordCnt,g_qCnt);
    // Write zero rows for all words
    uint8_t zero[MAX_QUESTIONS]={};
    for(int w=0;w<g_wordCnt;w++) f.write(zero,g_qCnt);
    f.close(); return true;
}

// Verify weights.bin header matches current word/question counts.
// Returns: 0=ok, 1=wrong magic or missing, 2=wrong qCnt,
//          3=wordCnt too small (need more rows),
//          4=wordCnt too large (need fewer rows),
//          5=file truncated (size doesn't match header)
int checkWeightFile(){
    File f=SD.open("/TwentyQ/weights.bin"); 
    if(!f){ Serial.println("[WGT] cannot open weights.bin → 1"); return 1; }
    uint32_t magic; int wc,qc;
    bool ok=(f.read((uint8_t*)&magic,4)==4&&magic==WEIGHT_MAGIC&&
             f.read((uint8_t*)&wc,4)==4&&
             f.read((uint8_t*)&qc,4)==4);
    size_t fileSize=f.size();
    f.close();
    Serial.printf("[WGT] file=%u bytes  magic=0x%08X  hdr_words=%d  hdr_q=%d  g_wordCnt=%d  g_qCnt=%d\n",
                  fileSize, magic, wc, qc, g_wordCnt, g_qCnt);
    if(!ok||magic!=WEIGHT_MAGIC){ Serial.println("[WGT] bad magic → 1"); return 1; }
    if(qc!=g_qCnt)              { Serial.printf("[WGT] qCnt mismatch %d vs %d → 2\n",qc,g_qCnt); return 2; }
    size_t expectedSize = WEIGHT_HDR + (size_t)wc * qc;
    if(fileSize < expectedSize) { Serial.printf("[WGT] truncated %u < %u → 5\n",fileSize,expectedSize); return 5; }
    if(wc<g_wordCnt)           { Serial.printf("[WGT] need more rows %d < %d → 3\n",wc,g_wordCnt); return 3; }
    if(wc>g_wordCnt)           { Serial.printf("[WGT] need fewer rows %d > %d → 4\n",wc,g_wordCnt); return 4; }
    Serial.println("[WGT] OK → 0");
    return 0;
}

// Resize weights.bin to match current g_wordCnt WITHOUT destroying existing data.
// If words.csv grew (new words added externally), append zero rows for the new words.
// If words.csv shrank (words removed externally), truncate by rewriting.
// Preserves all weights for words that still exist.
bool resizeWeightFile(int oldWordCnt){
    if(g_wordCnt > oldWordCnt){
        // words.csv has MORE words than weights.bin - append zero rows
        File f=SD.open("/TwentyQ/weights.bin",FILE_APPEND); if(!f)return false;
        uint8_t zero[MAX_QUESTIONS]={};
        int toAdd = g_wordCnt - oldWordCnt;
        for(int w=0;w<toAdd;w++) f.write(zero,g_qCnt);
        f.close();
        // Update header - "r+" opens read-write at position 0, no truncate
        File fh=SD.open("/TwentyQ/weights.bin","r+"); if(!fh)return false;
        writeWeightHeader(fh,g_wordCnt,g_qCnt);
        fh.close();
    } else if(g_wordCnt < oldWordCnt){
        // words.csv has FEWER words - copy kept rows to a temp file, then replace.
        File fr=SD.open("/TwentyQ/weights.bin"); if(!fr)return false;
        fr.seek(WEIGHT_HDR);
        File fw=SD.open("/TwentyQ/weights.tmp",FILE_WRITE); if(!fw){fr.close();return false;}
        writeWeightHeader(fw,g_wordCnt,g_qCnt);
        int w=0;
        while(w<g_wordCnt){
            int batch=min(CHUNK_SIZE,g_wordCnt-w);
            fr.read((uint8_t*)g_chunk,(size_t)batch*g_qCnt);
            fw.write((uint8_t*)g_chunk,(size_t)batch*g_qCnt);
            w+=batch;
        }
        fr.close(); fw.close();
        SD.remove("/TwentyQ/weights.bin");
        SD.rename("/TwentyQ/weights.tmp","/TwentyQ/weights.bin");
    }
    return true;
}

// ── Chunked scoring ───────────────────────────────────────────────────────────
int scoreAllWords(Candidate* top, int topN, int questionNum, bool showUI){
    if(showUI) showQuip(questionNum);

    File f = SD.open("/TwentyQ/weights.bin"); if(!f) return 0;
    f.seek(WEIGHT_HDR);

    for(int i = 0; i < topN; i++){ top[i].idx = -1; top[i].score = -9999.f; }
    int found = 0, w = 0;

    while(w < g_wordCnt){
        int batch = min(CHUNK_SIZE, g_wordCnt - w);
        f.read((uint8_t*)g_chunk, (size_t)batch * g_qCnt);

        for(int i = 0; i < batch; i++){
            float s = 0.f;
            for(int q = 0; q < g_qCnt; q++)
                if(g_answers[q] != ANS_NONE && g_answers[q] != ANS_IRRELEVANT)
                    s += g_answers[q] * ((float)g_chunk[i * g_qCnt + q] / 127.f);
            int wi = w + i;
            if(found < topN){
                top[found].idx = wi; top[found].score = s; found++;
            } else if(s > top[topN-1].score){
                top[topN-1].idx = wi; top[topN-1].score = s;
            } else continue;
            for(int k = (found < topN ? found-1 : topN-1); k > 0 && top[k].score > top[k-1].score; k--)
                { Candidate tmp = top[k]; top[k] = top[k-1]; top[k-1] = tmp; }
        }
        w += batch;
    }
    f.close();
    return found;
}

// Pick next question using variance-maximising scan.
// Only words that are still plausible (positive score vs current answers)
// contribute to the variance calculation, so after a category
// is selected, questions that are irrelevant to that category (e.g. "Does it
// lay eggs?" after "Object") never score high.
//
// Improved: when weights are cold (low variance), we use an information-gain
// heuristic that counts how many plausible words would answer YES vs NO,
// preferring questions that split the plausible set close to 50/50.
int pickNextQuestion(const bool* asked){
    float means[MAX_QUESTIONS]  = {};
    float means2[MAX_QUESTIONS] = {};
    int   posCount[MAX_QUESTIONS] = {};  // words with positive weight for this Q
    int   negCount[MAX_QUESTIONS] = {};  // words with negative weight for this Q
    int   plausible = 0;   // count of words still in contention

    bool hasAnswers = false;
    for(int q = 0; q < g_qCnt; q++)
        if(g_answers[q] != ANS_NONE){ hasAnswers = true; break; }

    File f = SD.open("/TwentyQ/weights.bin"); if(!f) return 0;
    f.seek(WEIGHT_HDR);

    int w = 0;
    while(w < g_wordCnt){
        int batch = min(CHUNK_SIZE, g_wordCnt - w);
        f.read((uint8_t*)g_chunk, (size_t)batch * g_qCnt);
        for(int i = 0; i < batch; i++){
            float score = 0.f;
            if(hasAnswers){
                for(int q = 0; q < g_qCnt; q++){
                    if(g_answers[q] == ANS_NONE || g_answers[q] == ANS_IRRELEVANT) continue;
                    score += g_answers[q] * ((float)g_chunk[i * g_qCnt + q] / 127.f);
                }
                // calculation meaningful even with a mostly-cold database.
                if(score < -1.0f) continue;
            }
            for(int q = 0; q < g_qCnt; q++){
                float v = (float)g_chunk[i * g_qCnt + q] / 127.f;
                means[q] += v; means2[q] += v * v;
                if(v > 0.1f)       posCount[q]++;
                else if(v < -0.1f) negCount[q]++;
            }
            plausible++;
        }
        w += batch;
    }
    f.close();

    if(plausible == 0) plausible = 1;   // avoid divide-by-zero

    int bestQ = -1; float bestScore = -1.f;
    int unasked[MAX_QUESTIONS]; int unaskedCnt = 0;
    float maxVar = 0.f;

    for(int q = 0; q < g_qCnt; q++){
        if(!asked[q]){
            float mean = means[q] / plausible;
            float var  = (means2[q] / plausible) - (mean * mean);
            if(var > maxVar) maxVar = var;
        }
    }

    for(int q = 0; q < g_qCnt; q++){
        if(!asked[q]){
            unasked[unaskedCnt++] = q;
            float mean = means[q] / plausible;
            float var  = (means2[q] / plausible) - (mean * mean);

            float score;
            if(maxVar > 0.01f){
                score = var;
            } else {
                int yesN = posCount[q];
                int noN  = negCount[q];
                int total = yesN + noN;
                if(total > 0){
                    float ratio = (float)min(yesN, noN) / (float)total;
                    score = ratio;  // 0.5 = perfect split, 0.0 = useless
                } else {
                    score = 0.f;
                }
            }

            score += (float)(random(100)) / 100000.f;   // tiny tie-break jitter
            if(score > bestScore){ bestScore = score; bestQ = q; }
        }
    }
    // If all scores are effectively zero (cold database or fully converged),
    // pick a random unasked question.
    if(bestScore < 0.001f && unaskedCnt > 0)
        bestQ = unasked[random(unaskedCnt)];
    return bestQ < 0 ? 0 : bestQ;
}

// ── Read word name by index ──────────────────────

bool readWordName(int idx, char* out, int outLen){
    File f=SD.open("/TwentyQ/words.csv"); if(!f)return false;
    int n=0;
    while(f.available()){
        String ln=f.readStringUntil('\n'); ln.trim();
        if(!ln.length()||ln[0]=='#') continue;
        int comma=ln.indexOf(','); String name=(comma>0)?ln.substring(0,comma):ln;
        name.trim();
        if(n==idx){ strncpy(out,name.c_str(),outLen-1); out[outLen-1]='\0'; f.close(); return true; }
        n++;
    }
    f.close(); return false;
}

// ── Update weights for one word ─────────────────────────────────────────────
void updateWordWeights(int wordIdx, bool isCorrect){
    File f=SD.open("/TwentyQ/weights.bin","r+");
    if(!f)return;

    // Read existing weights for this word
    uint32_t off=weightOffset(wordIdx);
    f.seek(off);
    int8_t row[MAX_QUESTIONS]={};
    f.read((uint8_t*)row,g_qCnt);

    float lr      = LEARN_RATES[g_cfg.learnRate];
    float lr_soft = LEARN_RATES_SOFT[g_cfg.learnRate];

    // Apply update
    for(int i=0;i<g_askedCnt;i++){
        int q=g_askedOrder[i];
        float ans=g_answers[q];
        if(ans==ANS_IRRELEVANT) continue;   // irrelevant = no learning signal
        float wf=(float)row[q]/127.f;
        float newW;
        if(isCorrect){
            newW=wf+lr*(ans-wf);
        } else {
            newW=wf+lr_soft*(-ans-wf);
        }
        if(newW>1.f)newW=1.f; if(newW<-1.f)newW=-1.f;
        row[q]=(int8_t)(newW*127.f);
    }

    // Write back
    f.seek(off);
    f.write((uint8_t*)row,g_qCnt);
    f.close();
}

// ── Full weight update (all words) ──────────────────────────────────────────
//
//   1. Correct word: strong pull toward answer (unchanged)
//   2. Wrong words with disagreeing weights: moderate pull toward -answer
//   3. Wrong words with AGREEING weights: small decay toward 0
void updateAllWeights(int correctIdx){
    screenInit("Updating...");
    bprint("Updating my brain.",C_DIM);
    bprintf(C_DIM,"(%d words)",g_wordCnt);

    float lr      = LEARN_RATES[g_cfg.learnRate];
    float lr_soft = LEARN_RATES_SOFT[g_cfg.learnRate];
    float lr_decay = lr_soft * 0.5f;   // very gentle decay for agreeing wrong words

    File f=SD.open("/TwentyQ/weights.bin","r+"); if(!f)return;
    f.seek(WEIGHT_HDR);

    int w=0;
    while(w<g_wordCnt){
        int batch=min(CHUNK_SIZE,g_wordCnt-w);
        uint32_t off=weightOffset(w);
        f.seek(off);
        f.read((uint8_t*)g_chunk,(size_t)batch*g_qCnt);

        for(int i=0;i<batch;i++){
            int wi=w+i;
            for(int j=0;j<g_askedCnt;j++){
                int q=g_askedOrder[j];
                float ans=g_answers[q];
                if(ans==ANS_IRRELEVANT) continue;   // no learning signal
                float wf=(float)g_chunk[i*g_qCnt+q]/127.f;
                float newW;
                if(wi==correctIdx){
                    newW=wf+lr*(ans-wf);
                } else {
                    if(ans*wf<0.f){
                        newW=wf+lr_soft*(-ans-wf);
                    } else if(fabsf(wf) > 0.01f) {
                        newW=wf*(1.f - lr_decay);
                    } else {
                        newW=wf;  // near-zero, leave alone
                    }
                }
                if(newW>1.f)newW=1.f; if(newW<-1.f)newW=-1.f;
                g_chunk[i*g_qCnt+q]=(int8_t)(newW*127.f);
            }
        }
        f.seek(off);
        f.write((uint8_t*)g_chunk,(size_t)batch*g_qCnt);
        w+=batch;
    }
    f.close();
}

// ── Append a new word to both words.csv and weights.bin ───────────────────────
bool appendNewWord(const char* name){
    // Append name to words.csv
    File fc=SD.open("/TwentyQ/words.csv",FILE_APPEND); if(!fc)return false;
    fc.println(name); fc.close();

    // Append seeded weight row to weights.bin
    File fw=SD.open("/TwentyQ/weights.bin",FILE_APPEND); if(!fw)return false;
    int8_t row[MAX_QUESTIONS]={};
    for(int q=0;q<g_qCnt;q++) row[q]=(int8_t)(g_answers[q]*127.f);
    fw.write((uint8_t*)row,g_qCnt);
    fw.close();

    // Update header wordCnt - "r+" opens read-write at position 0, no truncate
    g_wordCnt++;
    File fh=SD.open("/TwentyQ/weights.bin","r+"); if(!fh)return false;
    writeWeightHeader(fh,g_wordCnt,g_qCnt);
    fh.close();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CSV LOADING  (questions + word count only - names are read on demand)
// ═══════════════════════════════════════════════════════════════════════════════
bool loadQuestions(){
    g_qCnt=0;
    File f=SD.open("/TwentyQ/questions.csv"); if(!f)return false;
    while(f.available()&&g_qCnt<MAX_QUESTIONS){
        String ln=f.readStringUntil('\n'); ln.trim();
        if(!ln.length()||ln[0]=='#') continue;
        if(ln.length()<Q_LEN) strncpy(g_questions[g_qCnt++],ln.c_str(),Q_LEN-1);
    }
    f.close(); return g_qCnt>0;
}

// Count words in words.csv (fast scan, no name storage)
int countWords(){
    int n=0;
    File f=SD.open("/TwentyQ/words.csv"); if(!f)return 0;
    while(f.available()){
        String ln=f.readStringUntil('\n'); ln.trim();
        if(!ln.length()||ln[0]=='#') continue;
        n++;
    }
    f.close(); return n;
}

bool loadAll(){
    if(!loadQuestions()) return false;
    g_wordCnt=countWords(); if(!g_wordCnt)return false;
    Serial.printf("[LOAD] %d questions, %d words\n", g_qCnt, g_wordCnt);

    // Resolve category question indices from the loaded question list
    buildCategories();

    // Check/create weights.bin
    int wc=checkWeightFile();
    Serial.printf("[LOAD] checkWeightFile → %d\n", wc);
    if(wc==1||wc==2||wc==5){
        // Missing, incompatible question count, or truncated - must create fresh
        Serial.printf("[LOAD] CREATING FRESH weights (reason=%d)\n", wc);
        createWeightFile();
    } else if(wc==3||wc==4){
        // Word count mismatch - resize preserving existing weights
        File f=SD.open("/TwentyQ/weights.bin"); 
        if(f){
            uint32_t mag; int oldWC,oldQC;
            f.read((uint8_t*)&mag,4);
            f.read((uint8_t*)&oldWC,4);
            f.read((uint8_t*)&oldQC,4);
            f.close();
            Serial.printf("[LOAD] RESIZING weights %d → %d words\n", oldWC, g_wordCnt);
            resizeWeightFile(oldWC);
        } else {
            Serial.println("[LOAD] CREATING FRESH weights (resize open failed)");
            createWeightFile();
        }
    } else {
        Serial.println("[LOAD] weights OK, no changes needed");
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CATEGORY PICKER  -  2×3 grid matching answer picker style
//
//  Row 0:  Animal  | Plant  | Object
//  Row 1:  Concept | ?      | (empty - wraps to Not sure)
//
//  5 categories arranged as 2 rows x 3 cols; slot [1][2] = "Not sure"
//
//  the category picker.
// ═══════════════════════════════════════════════════════════════════════════════
struct Category {
    const char* label;
    uint16_t    colour;
    // Filled by buildCategoryIndices() - -1 = not found / unused
    int         yesQ[4];
    int         noQ[4];
};

// Substring match (case-insensitive) to locate question indices.
// changes in questions.csv don't break things.
static const char* CAT_Q_ANIMAL  = "Is it an animal";
static const char* CAT_Q_PLANT   = "Is it a plant";
static const char* CAT_Q_LIVING  = "Is it a living thing";
static const char* CAT_Q_MANMADE = "Is it man-made";

static int g_qAnimal  = -1;
static int g_qPlant   = -1;
static int g_qLiving  = -1;
static int g_qManmade = -1;

// Resolve category question indices from loaded g_questions[]
static void buildCategoryIndices(){
    g_qAnimal = g_qPlant = g_qLiving = g_qManmade = -1;
    for(int i = 0; i < g_qCnt; i++){
        if(g_qAnimal  < 0 && strncasecmp(g_questions[i], CAT_Q_ANIMAL,  strlen(CAT_Q_ANIMAL )) == 0) g_qAnimal  = i;
        if(g_qPlant   < 0 && strncasecmp(g_questions[i], CAT_Q_PLANT,   strlen(CAT_Q_PLANT  )) == 0) g_qPlant   = i;
        if(g_qLiving  < 0 && strncasecmp(g_questions[i], CAT_Q_LIVING,  strlen(CAT_Q_LIVING )) == 0) g_qLiving  = i;
        if(g_qManmade < 0 && strncasecmp(g_questions[i], CAT_Q_MANMADE, strlen(CAT_Q_MANMADE)) == 0) g_qManmade = i;
    }
}

static Category CATEGORIES[] = {
    { "Animal",   C_OK,    {-1,-1,-1,-1}, {-1,-1,-1,-1} },
    { "Plant",    0x47E0u, {-1,-1,-1,-1}, {-1,-1,-1,-1} },
    { "Object",   C_WARN,  {-1,-1,-1,-1}, {-1,-1,-1,-1} },
    { "Concept",  C_TITFG, {-1,-1,-1,-1}, {-1,-1,-1,-1} },
    { "Not sure", C_DIM,   {-1,-1,-1,-1}, {-1,-1,-1,-1} },
};

static void buildCategories(){
    buildCategoryIndices();
    buildExclusions();
    int A=g_qAnimal, P=g_qPlant, L=g_qLiving, M=g_qManmade;
    // Animal:  yes=animal,living    no=plant,man-made
    CATEGORIES[0] = { "Animal",   C_OK,    { A, L,-1,-1}, { P, M,-1,-1} };
    // Plant:   yes=plant,living     no=animal,man-made
    CATEGORIES[1] = { "Plant",    0x47E0u, { P, L,-1,-1}, { A, M,-1,-1} };
    // Object:  yes=man-made         no=animal,plant,living
    CATEGORIES[2] = { "Object",   C_WARN,  { M,-1,-1,-1}, { A, P, L,-1} };
    // Concept: no=animal,plant,living,man-made
    CATEGORIES[3] = { "Concept",  C_TITFG, {-1,-1,-1,-1}, { A, P, L, M} };
    // Not sure: no pre-fills
    CATEGORIES[4] = { "Not sure", C_DIM,   {-1,-1,-1,-1}, {-1,-1,-1,-1} };
}
#define CAT_COUNT  5
#define CAT_COLS   3
#define CAT_ROWS   2

// Grid layout: [row][col] → category index (-1 = empty)
static const int CAT_GRID[CAT_ROWS][CAT_COLS] = {
    { 0, 1, 2 },   // Animal | Plant | Object
    { 3, 4,-1 },   // Concept | Not sure | (empty)
};

// ═══════════════════════════════════════════════════════════════════════════════
//  QUESTION EXCLUSION SYSTEM
// ═══════════════════════════════════════════════════════════════════════════════

struct ExclusionRule {
    int  triggerQ;        // question index that triggers this rule
    bool onYes;           // true = fires when answer >= ANS_PROBABLY_YES
                          // false = fires when answer <= ANS_PROBABLY_NO
    int  targets[12];     // question indices to mark as asked; -1 = end
};

static ExclusionRule* g_exclusions     = nullptr;
static int            g_exclusionCount = 0;

// Text fragments used to locate questions. Must be unambiguous prefixes.
// Each entry: { trigger_fragment, onYes, { target_fragments... } }
struct RuleSpec {
    const char* trigger;
    bool        onYes;
    const char* targets[12];   // null-terminated list
};

static const RuleSpec RULE_SPECS[] = {

    // ── CATEGORY: not an animal → exclude all biology questions ──────────────
    { "Is it an animal",        false,  // NO: not an animal
        { "Does it have legs",
          "Does it have wings",
          "Does it have fur",
          "Does it have scales",
          "Does it live in water",
          "Does it live underground",
          "Can it move on its own",
          "Does it have a face",
          "Does it lay eggs",
          nullptr } },

    // ── CATEGORY: is an animal → exclude man-made / object questions ─────────
    { "Is it an animal",        true,   // YES: is an animal
        { "Is it man-made",
          "Is it a type of food",
          "Is it a type of drink",
          "Is it a vehicle",
          "Is it a tool or instrument",
          "Is it a toy or game",
          "Is it a piece of clothing",
          "Is it a piece of furniture",
          "Is it a building or structure",
          "Is it a musical instrument",
          "Is it an electronic device",
          nullptr } },

    // ── CATEGORY: not a plant → exclude plant biology ────────────────────────
    { "Is it a plant",          false,  // NO: not a plant
        { "Does it have roots",
          "Does it have flowers",
          "Does it have leaves",
          nullptr } },

    // ── CATEGORY: is a plant → exclude animal/object questions ───────────────
    { "Is it a plant",          true,   // YES: is a plant
        { "Is it man-made",
          "Does it have legs",
          "Does it have wings",
          "Does it have fur",
          "Does it have scales",
          "Can it move on its own",
          "Does it have a face",
          "Does it lay eggs",
          "Is it a vehicle",
          "Is it a tool or instrument",
          "Is it an electronic device",
          nullptr } },

    // ── CATEGORY: not living → exclude all biology ───────────────────────────
    { "Is it a living thing",   false,  // NO: not living
        { "Does it have legs",
          "Does it have wings",
          "Does it have fur",
          "Does it have scales",
          "Does it live in water",
          "Does it live underground",
          "Can it move on its own",
          "Does it have a face",
          "Does it lay eggs",
          "Does it have roots",
          "Does it have flowers",
          "Does it have leaves" } },   // no nullptr needed - 12 items fills array

    // ── SIZE: bigger than a person → exclude small-size questions ────────────
    { "Is it bigger than a person", true,
        { "Is it smaller than a tennis ball",
          "Can you hold it in one hand",
          "Can you carry it in a pocket",
          "Is it tiny (smaller than a coin)",
          nullptr } },

    // ── SIZE: bigger than a car → exclude all smaller-size questions ─────────
    { "Is it bigger than a car", true,
        { "Is it bigger than a loaf of bread",   // already implied
          "Is it smaller than a tennis ball",
          "Can you hold it in one hand",
          "Can you carry it in a pocket",
          "Is it tiny (smaller than a coin)",
          nullptr } },

    // ── SIZE: smaller than a tennis ball → exclude large-size questions ──────
    { "Is it smaller than a tennis ball", true,
        { "Is it bigger than a person",
          "Is it bigger than a car",
          "Is it bigger than a loaf of bread",
          nullptr } },

    // ── SIZE: fits in a pocket → exclude large-size questions ────────────────
    { "Can you carry it in a pocket", true,
        { "Is it bigger than a person",
          "Is it bigger than a car",
          "Is it bigger than a loaf of bread",
          nullptr } },

    // ── SIZE: tiny (smaller than a coin) → exclude all larger sizes ──────────
    { "Is it tiny (smaller than a coin)", true,
        { "Is it bigger than a person",
          "Is it bigger than a car",
          "Is it bigger than a loaf of bread",
          "Is it smaller than a tennis ball",   // implied - already known
          "Can you hold it in one hand",         // implied
          nullptr } },

    // ── SIZE: NOT bigger than a loaf → exclude car/person sizes ─────────────
    { "Is it bigger than a loaf of bread", false,
        { "Is it bigger than a person",
          "Is it bigger than a car",
          nullptr } },

    // ── SIZE: NOT smaller than tennis ball → exclude pocket/tiny ─────────────
    { "Is it smaller than a tennis ball", false,
        { "Can you carry it in a pocket",
          "Is it tiny (smaller than a coin)",
          nullptr } },

    // ── SIZE: can hold in hand (yes) → exclude car/person sizes ─────────────
    { "Can you hold it in one hand",  true,
        { "Is it bigger than a person",
          "Is it bigger than a car",
          nullptr } },

    // ── SIZE: cannot hold in hand → exclude pocket/coin ──────────────────────
    { "Can you hold it in one hand",  false,
        { "Can you carry it in a pocket",
          "Is it tiny (smaller than a coin)",
          nullptr } },

    // ── MATERIAL: is hard → exclude soft ─────────────────────────────────────
    { "Is it hard",   true,  { "Is it soft", nullptr } },
    { "Is it soft",   true,  { "Is it hard", nullptr } },

    // ── WEIGHT: heavy → exclude light ────────────────────────────────────────
    { "Is it heavy",              true,  { "Is it light (not heavy)", nullptr } },
    { "Is it light (not heavy)",  true,  { "Is it heavy", nullptr } },

    // ── TEXTURE: rough → exclude smooth ──────────────────────────────────────
    { "Is it rough to the touch", true,  { "Is it smooth to the touch", nullptr } },
    { "Is it smooth to the touch",true,  { "Is it rough to the touch",  nullptr } },

    // ── ELECTRICITY: uses electricity → can't be older than 100 years (mostly)
    // (skipped - some old electric things exist, too many false positives)

    // ── WORN: is worn on body → not furniture, not vehicle, not building ──────
    { "Is it worn on the body",   true,
        { "Is it a piece of furniture",
          "Is it a vehicle",
          "Is it a building or structure",
          nullptr } },

    // Sentinel
    { nullptr, false, { nullptr } }
};

// Resolve all rule specs into ExclusionRule[] using loaded g_questions[].
// Called from buildCategories() after questions are loaded.
static void buildExclusions(){
    // Count rules
    int nSpecs = 0;
    while(RULE_SPECS[nSpecs].trigger) nSpecs++;

    // Allocate (static array on heap - done once per loadAll)
    static ExclusionRule ruleBuf[64];   // enough for all rules above
    g_exclusionCount = 0;

    // Helper: find question index by text prefix
    auto findQ = [](const char* frag) -> int {
        if(!frag) return -1;
        int flen = strlen(frag);
        for(int i = 0; i < g_qCnt; i++)
            if(strncasecmp(g_questions[i], frag, flen) == 0) return i;
        return -1;
    };

    for(int s = 0; s < nSpecs && g_exclusionCount < 64; s++){
        int trig = findQ(RULE_SPECS[s].trigger);
        if(trig < 0) continue;   // question not in CSV - skip rule

        ExclusionRule& r = ruleBuf[g_exclusionCount];
        r.triggerQ = trig;
        r.onYes    = RULE_SPECS[s].onYes;
        for(int t = 0; t < 12; t++){
            r.targets[t] = findQ(RULE_SPECS[s].targets[t]);
        }
        g_exclusionCount++;
    }
    g_exclusions = ruleBuf;
}

static void applyExclusions(int qi, float ansVal, bool* asked){
    if(!g_exclusions) return;
    bool isYes = (ansVal >=  0.4f);
    bool isNo  = (ansVal <= -0.4f);
    if(!isYes && !isNo) return;   // Unknown / ambiguous - no exclusions

    for(int r = 0; r < g_exclusionCount; r++){
        if(g_exclusions[r].triggerQ != qi) continue;
        if(g_exclusions[r].onYes != isYes) continue;
        // Fire: mark all targets as asked (silently skipped in UI)
        // but set a default implied answer so scoring/learning can use them.
        for(int t = 0; t < 12; t++){
            int tq = g_exclusions[r].targets[t];
            if(tq < 0) break;
            if(!asked[tq]){
                asked[tq]     = true;
                g_answers[tq] = ANS_NO;   // implied "no" - participates in scoring+learning
                g_askedOrder[g_askedCnt++] = tq;  // included in learning pass
            }
        }
    }
}

int askCategory(bool* asked){
    int row=0, col=0;

    auto drawCell=[&](int r, int c, bool hi){
        int ci = CAT_GRID[r][c];
        int CW = DW / CAT_COLS;
        int CH = (DH - BODYY) / CAT_ROWS;
        int x  = c * CW;
        int y  = BODYY + r * CH;
        if(ci<0){
            M5Cardputer.Display.fillRect(x,y,CW-1,CH-1,C_BG);
            return;
        }
        uint16_t ac = CATEGORIES[ci].colour;
        uint16_t bg = hi ? ac  : C_BG;
        uint16_t fg = hi ? C_BG : ac;
        M5Cardputer.Display.fillRect(x,y,CW-1,CH-1,bg);
        M5Cardputer.Display.drawRect(x,y,CW-1,CH-1,hi?C_FG:C_DIM);
        int lw = strlen(CATEGORIES[ci].label)*6;
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setTextColor(fg,bg);
        M5Cardputer.Display.setCursor(x+(CW-1-lw)/2, y+(CH-1-8)/2);
        M5Cardputer.Display.print(CATEGORIES[ci].label);
    };

    auto draw=[&](){
        M5Cardputer.Display.fillScreen(C_BG);
        titleBar("What is it?");
        for(int r=0;r<CAT_ROWS;r++)
            for(int c=0;c<CAT_COLS;c++)
                drawCell(r,c,(r==row&&c==col));
    };

    draw();
    while(true){
        char ch=waitCh();
        int pr=row, pc=col;
        if(ch==KUP   && row>0)          row--;
        else if(ch==KDOWN&&row<CAT_ROWS-1) row++;
        else if(ch==KLEFT&&col>0)          col--;
        else if(ch==KRIGHT&&col<CAT_COLS-1) col++;
        else if(ch=='\r'){
            int ci=CAT_GRID[row][col];
            if(ci<0) continue;   // empty cell
            // Apply answers
            const Category& cat=CATEGORIES[ci];
            for(int i=0;i<4;i++){
                if(cat.yesQ[i]>=0&&cat.yesQ[i]<g_qCnt){
                    g_answers[cat.yesQ[i]]=ANS_YES; asked[cat.yesQ[i]]=true;
                    g_askedOrder[g_askedCnt++]=cat.yesQ[i];
                    applyExclusions(cat.yesQ[i], ANS_YES, asked);
                }
                if(cat.noQ[i]>=0&&cat.noQ[i]<g_qCnt){
                    g_answers[cat.noQ[i]]=ANS_NO; asked[cat.noQ[i]]=true;
                    g_askedOrder[g_askedCnt++]=cat.noQ[i];
                    applyExclusions(cat.noQ[i], ANS_NO, asked);
                }
            }
            return ci;
        }
        // Clamp to valid cell - skip empty [1][2]
        if(CAT_GRID[row][col]<0){ row=pr; col=pc; }
        if(row!=pr||col!=pc) draw();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  MAIN GAME
// ═══════════════════════════════════════════════════════════════════════════════
void runGame(){
    if(!g_wordCnt||!g_qCnt){
    screenInit("Error");
        bprint(g_wordCnt?"No questions!":"No words!",C_ERR);
        bprint("Check SD card.",C_DIM); waitCh(); return;
    }

    // Intro
    screenInit("20 Questions");
    M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_TITFG,C_BG);
    M5Cardputer.Display.setCursor(4,BODYY+2);      M5Cardputer.Display.print("Think of a word.");
    M5Cardputer.Display.setCursor(4,BODYY+2+LH);   M5Cardputer.Display.print("I'll guess it");
    M5Cardputer.Display.setCursor(4,BODYY+2+LH*2); M5Cardputer.Display.print("in 20 questions!");
    M5Cardputer.Display.setTextSize(1); M5Cardputer.Display.setTextColor(C_DIM,C_BG);
    M5Cardputer.Display.setCursor(4,BODYY+2+LH*3+4);
    M5Cardputer.Display.printf("(%d words, %d questions)",g_wordCnt,g_qCnt);
    waitCh();

    // Init
    for(int q = 0; q < g_qCnt; q++) g_answers[q] = ANS_NONE;
    bool asked[MAX_QUESTIONS];
    memset(asked, 0, sizeof(asked));
    g_askedCnt = 0;

    // Category picker - acts as Q0, pre-fills several answers at once
    askCategory(asked);

    // Ask exactly MAX_Q_PER_GAME questions - no early guessing
    for(int turn=0;turn<MAX_Q_PER_GAME;turn++){
        // Show a personality quip at ~5 dramatic moments per game:
        // turns 4, 8, 12, 16, 19 - spread across early/mid/late phases.
        // The quip screen also masks the pickNextQuestion chunk scan.
        // All other turns go straight to the question with no delay.
        bool showQ = (turn==3||turn==7||turn==11||turn==15||turn==18);
        if(showQ) showQuip(turn + 1);

        int qi = pickNextQuestion(asked);
        if(qi < 0) break;

        // If we showed a quip, the SD load has now finished - wait for keypress
        // before moving on to the question screen.
        if(showQ) waitForQuipDismiss();

        bool quit=false;
        float ansVal=askQuestion(g_questions[qi],turn+1,MAX_Q_PER_GAME,&quit);
        if(quit){
            if(!g_cfg.confirmQuit || confirmDialog("Quit?","End this game?",false)) return;
            turn--; continue;
        }
        g_answers[qi] = ansVal;
        asked[qi]     = true;
        // Irrelevant doesn't count as a meaningful answer - skip adding to askedOrder
        if(ansVal != ANS_IRRELEVANT){
            g_askedOrder[g_askedCnt++] = qi;
        }
        // Mark logically contradicted questions as irrelevant
        applyExclusions(qi, ansVal, asked);
    }

    // All 20 questions done - dramatic pre-reveal screen
    M5Cardputer.Display.fillScreen(C_BG);
    titleBar("20 questions up!");
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(C_TITFG, C_BG);
    M5Cardputer.Display.setCursor(4, BODYY + 4);
    M5Cardputer.Display.print("I have reached");
    M5Cardputer.Display.setCursor(4, BODYY + 4 + LH);
    M5Cardputer.Display.print("my conclusion.");

    Candidate top[TOP_N];
    int found = scoreAllWords(top, TOP_N, 21, false);

    waitForQuipDismiss();

    // Guess top candidates
    bool guessed=false; int guessedAt=-1;
    for(int g=0;g<found&&!guessed;g++){
        char dispName[WORD_LEN]={};
        readWordName(top[g].idx,dispName,WORD_LEN);
        for(int i=0;dispName[i];i++) if(dispName[i]=='_') dispName[i]=' ';

        float conf=(g_askedCnt>0)?(top[g].score/(float)g_askedCnt*50.f+50.f):50.f;
        if(conf>99.f)conf=99.f; if(conf<1.f)conf=1.f;

        M5Cardputer.Display.fillScreen(C_BG);
        titleBar(g==0?"My best guess...":"Hmm, maybe...");
        M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_WARN,C_BG);
        M5Cardputer.Display.setCursor(4,BODYY+4);
        M5Cardputer.Display.print(g==0?"I think it's...":"Or is it...");

        M5Cardputer.update();

        for(int dot=0;dot<3;dot++){
            M5Cardputer.Display.setTextColor(C_TITFG,C_BG);
            M5Cardputer.Display.setCursor(4+dot*16,BODYY+4+LH);
            M5Cardputer.Display.print("."); delay(440);
        }
        M5Cardputer.Display.fillRect(0,BODYY+4+LH,DW,LH*2,C_BG);
        M5Cardputer.Display.setTextColor(C_TITFG,C_BG);
        M5Cardputer.Display.setCursor(4,BODYY+4+LH);
        M5Cardputer.Display.print(dispName); M5Cardputer.Display.print("?");
        int barW = (int)(conf / 100.f * (DW - 8));
        M5Cardputer.Display.fillRect(4, BODYY + 4 + LH * 2 + 6, barW, 5, C_OK);
        M5Cardputer.Display.drawRect(4, BODYY + 4 + LH * 2 + 6, DW - 8, 5, C_DIM);
        M5Cardputer.Display.setTextSize(1); M5Cardputer.Display.setTextColor(C_DIM, C_BG);
        M5Cardputer.Display.setCursor(4, BODYY + 4 + LH * 2 + 13);
        M5Cardputer.Display.printf("Confidence: %d", (int)conf); M5Cardputer.Display.print("%");

        delay(800);
        M5Cardputer.update();

                int btnY  = BODYY + 4 + LH * 3 + 6;
        int yesX  = 18, noX = 136;
        int btnW  = 84, btnH = LH + 4;
        int ynSel = 1;   // default to NO - safer

        auto drawYN = [&](){
            uint16_t yb = (ynSel == 0) ? C_OK  : C_DIM;
            uint16_t nb = (ynSel == 1) ? C_ERR : C_DIM;
            M5Cardputer.Display.fillRoundRect(yesX, btnY, btnW, btnH, 4, yb);
            M5Cardputer.Display.fillRoundRect(noX,  btnY, btnW, btnH, 4, nb);
            M5Cardputer.Display.setTextSize(2);
            M5Cardputer.Display.setTextColor(C_BG, yb);
            M5Cardputer.Display.setCursor(yesX + 24, btnY + 2);
            M5Cardputer.Display.print("YES");
            M5Cardputer.Display.setTextColor(C_BG, nb);
            M5Cardputer.Display.setCursor(noX + 30, btnY + 2);
            M5Cardputer.Display.print("NO");
        };

        drawYN();
        bool correct = false;
        while(true){
            char r = waitCh();
            if(r == KLEFT || r == KRIGHT){ ynSel = 1 - ynSel; drawYN(); }
            else if(r == KUP || r == KDOWN){ ynSel = 1 - ynSel; drawYN(); }
            else if(r == '\r'){ correct = (ynSel == 0); break; }
            else if(r == '\b'){ correct = false; break; }
        }

        if(correct){
            guessed=true; guessedAt=top[g].idx;
        } else {
            // Wrong guess - apply negative learning to push this word away
            // from the current answer pattern. This helps differentiate
            // similar words (e.g. computer vs laptop) over multiple games.
            updateWordWeights(top[g].idx, false);
        }

        if(guessed){
            beep(1800,80); delay(100); beep(2400,80);
            g_stats.computerWins++;
            g_stats.gamesPlayed++;
            g_stats.totalQuestions += g_askedCnt;
            g_stats.currentStreak  = (g_stats.currentStreak>0) ? g_stats.currentStreak+1 : 1;
            if(g_stats.currentStreak > g_stats.bestStreak) g_stats.bestStreak=g_stats.currentStreak;
            saveStats();
                       screenInit("Got it!");
            M5Cardputer.Display.setTextSize(2);
            M5Cardputer.Display.setTextColor(C_OK,   C_BG); M5Cardputer.Display.setCursor(4,BODYY+4);    M5Cardputer.Display.print("Yes! I knew it!");
            M5Cardputer.Display.setTextColor(C_TITFG,C_BG); M5Cardputer.Display.setCursor(4,BODYY+4+LH); M5Cardputer.Display.print(dispName);
            M5Cardputer.Display.setTextSize(1);
            M5Cardputer.Display.setTextColor(C_DIM,  C_BG); M5Cardputer.Display.setCursor(4,BODYY+4+LH*2+4);
            M5Cardputer.Display.printf("Confidence was: %d",(int)conf); M5Cardputer.Display.print("%");
            waitCh();
        }
    }

    // ── Defeat screen ─────────────────────────────────────────────────────────
    if(!guessed){
        char bestName[WORD_LEN]={};
        readWordName(top[0].idx,bestName,WORD_LEN);
        for(int i=0;bestName[i];i++) if(bestName[i]=='_') bestName[i]=' ';
        float bestConf=(g_askedCnt>0)?(top[0].score/(float)g_askedCnt*50.f+50.f):50.f;
        if(bestConf>99.f)bestConf=99.f; if(bestConf<1.f)bestConf=1.f;

        M5Cardputer.Display.fillScreen(C_BG); titleBar("You stumped me!");
        g_stats.playerWins++;
        g_stats.gamesPlayed++;
        g_stats.totalQuestions += g_askedCnt;
        g_stats.currentStreak  = (g_stats.currentStreak<0) ? g_stats.currentStreak-1 : -1;
        if(-g_stats.currentStreak > g_stats.bestPlayerStreak) g_stats.bestPlayerStreak=-g_stats.currentStreak;
        saveStats();
        M5Cardputer.Display.setTextSize(2);
        M5Cardputer.Display.setTextColor(C_WARN,C_BG); M5Cardputer.Display.setCursor(4,BODYY+2);    M5Cardputer.Display.print("I couldn't get it.");
        M5Cardputer.Display.setTextColor(C_DIM, C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH); M5Cardputer.Display.print("My best guess was:");
        M5Cardputer.Display.setTextColor(C_ERR, C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH*2); M5Cardputer.Display.print(bestName);
        int barW=(int)(bestConf/100.f*(DW-8));
        M5Cardputer.Display.fillRect(4,BODYY+2+LH*3+2,barW,5,C_ERR);
        M5Cardputer.Display.drawRect(4,BODYY+2+LH*3+2,DW-8,5,C_DIM);
        M5Cardputer.Display.setTextSize(1); M5Cardputer.Display.setTextColor(C_DIM,C_BG);
        M5Cardputer.Display.setCursor(4,BODYY+2+LH*3+10);
        M5Cardputer.Display.printf("Confidence: %d",(int)bestConf); M5Cardputer.Display.print("% (not great!)");
        waitCh();
    }

    // ── Learning ──────────────────────────────────────────────────────────────
    if(guessed){
        // Full update pass - all words get nudged, correct word gets strong update
        updateAllWeights(guessedAt);
    } else {
        M5Cardputer.Display.fillScreen(C_BG); titleBar("Teach me!");
        M5Cardputer.Display.setTextSize(2); M5Cardputer.Display.setTextColor(C_TITFG, C_BG);
        M5Cardputer.Display.setCursor(4, BODYY+2);      M5Cardputer.Display.print("What was your");
        M5Cardputer.Display.setCursor(4, BODYY+2+LH);   M5Cardputer.Display.print("word? I want");
        M5Cardputer.Display.setCursor(4, BODYY+2+LH*2); M5Cardputer.Display.print("to learn it!");

                int tmSel = 1;   // default NO
        int btnY  = BODYY + 2 + LH * 3 + 6;
        auto drawTM = [&](){
            uint16_t yb = (tmSel == 0) ? C_OK  : C_DIM;
            uint16_t nb = (tmSel == 1) ? C_ERR : C_DIM;
            M5Cardputer.Display.fillRoundRect(18,  btnY, 84, LH+4, 4, yb);
            M5Cardputer.Display.fillRoundRect(136, btnY, 84, LH+4, 4, nb);
            M5Cardputer.Display.setTextSize(2);
            M5Cardputer.Display.setTextColor(C_BG, yb);
            M5Cardputer.Display.setCursor(44,  btnY + 2); M5Cardputer.Display.print("YES");
            M5Cardputer.Display.setTextColor(C_BG, nb);
            M5Cardputer.Display.setCursor(164, btnY + 2); M5Cardputer.Display.print("NO");
        };
        drawTM();
        bool wantTeach = false;
        while(true){
            char r = waitCh();
            if(r == KLEFT || r == KRIGHT || r == KUP || r == KDOWN){ tmSel = 1 - tmSel; drawTM(); }
            else if(r == '\r'){ wantTeach = (tmSel == 0); break; }
            else if(r == '\b'){ break; }
        }

        if(wantTeach){
            String name=typeText("What's the word?","Type it and press Enter");
            name.trim(); name.replace(' ','_');
            if(name.length()>0&&name.length()<WORD_LEN){
                // Check if already known - scan words.csv
                int existingIdx=-1;
                File fc=SD.open("/TwentyQ/words.csv"); int n=0;
                while(fc.available()){
                    String ln=fc.readStringUntil('\n');ln.trim();
                    if(!ln.length()||ln[0]=='#') continue;
                    int comma=ln.indexOf(','); String wn=(comma>0)?ln.substring(0,comma):ln;
                    wn.trim(); String inp=name; inp.toLowerCase(); wn.toLowerCase();
                    if(inp==wn){existingIdx=n;break;}
                    n++;
                }
                fc.close();

                if(existingIdx>=0){
                    // Already known - just update its weights with this game
                    updateWordWeights(existingIdx,true);
                    char dispName[WORD_LEN]={};
                    readWordName(existingIdx,dispName,WORD_LEN);
                    for(int i=0;dispName[i];i++) if(dispName[i]=='_') dispName[i]=' ';
                    M5Cardputer.Display.fillScreen(C_BG); titleBar("Oh! I know that!");
                    M5Cardputer.Display.setTextSize(2);
                    M5Cardputer.Display.setTextColor(C_OK,  C_BG); M5Cardputer.Display.setCursor(4,BODYY+2);      M5Cardputer.Display.print(dispName);
                    M5Cardputer.Display.setTextColor(C_WARN,C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH);   M5Cardputer.Display.print("is already in my");
                    M5Cardputer.Display.setTextColor(C_WARN,C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH*2); M5Cardputer.Display.print("brain - I just");
                    M5Cardputer.Display.setTextColor(C_WARN,C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH*3); M5Cardputer.Display.print("guessed badly.");
                    M5Cardputer.Display.setTextColor(C_DIM, C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH*4); M5Cardputer.Display.print("I'll do better!");
                    { char k; do { k = waitCh(); } while(k != '\r' && k != '\b'); }
                } else {
                    // Brand new word
                    if(appendNewWord(name.c_str())){
                        char dispName[WORD_LEN]; strncpy(dispName,name.c_str(),WORD_LEN-1);
                        for(int i=0;dispName[i];i++) if(dispName[i]=='_') dispName[i]=' ';
                        strncpy(g_stats.lastWord, dispName, WORD_LEN-1); saveStats();
                        M5Cardputer.Display.fillScreen(C_BG); titleBar("Got it!");
                        M5Cardputer.Display.setTextSize(2);
                        M5Cardputer.Display.setTextColor(C_OK, C_BG); M5Cardputer.Display.setCursor(4,BODYY+2);      M5Cardputer.Display.print(dispName);
                        M5Cardputer.Display.setTextColor(C_FG, C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH);   M5Cardputer.Display.print("added to my brain!");
                        M5Cardputer.Display.setTextColor(C_DIM,C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH*2); M5Cardputer.Display.printf("I now know %d",g_wordCnt);
                        M5Cardputer.Display.setTextColor(C_DIM,C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH*3); M5Cardputer.Display.print("words. Next time");
                        M5Cardputer.Display.setTextColor(C_DIM,C_BG); M5Cardputer.Display.setCursor(4,BODYY+2+LH*4); M5Cardputer.Display.print("I'll get you!");
                        beep(1200,60); delay(80); beep(1600,60); delay(80); beep(2000,80);
                        { char k; do { k = waitCh(); } while(k != '\r' && k != '\b'); }
                    }
                }
            }
        }
    }
}

static void statRow(int x, int y, const char* label, const char* val, uint16_t vc){
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(C_DIM,C_BG);
    M5Cardputer.Display.setCursor(x,y);
    M5Cardputer.Display.print(label);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(vc,C_BG);
    M5Cardputer.Display.setCursor(x,y+9);
    M5Cardputer.Display.print(val);
}

// Page dots helper
static void pageDots(int cur, int total){
    for(int i=0;i<total;i++)
        M5Cardputer.Display.fillCircle(DW-4-(total-1-i)*10, TITLEH/2, 3, i==cur?C_FG:C_DIM);
}

void statsScreen(){
    const int PAGES=3;
    int page=0;
    while(true){
        M5Cardputer.Display.fillScreen(C_BG);
        char pbuf[18]; snprintf(pbuf,18,"Stats %d/%d",page+1,PAGES);
        titleBar(pbuf);
        pageDots(page,PAGES);
        char buf[32];

        if(page==0){
            // Win/loss overview
            int played=g_stats.gamesPlayed, cW=g_stats.computerWins, pW=g_stats.playerWins;
            int pct=played>0?(cW*100/played):0;
            int avgQ=played>0?(g_stats.totalQuestions/played):0;

            statRow(4,   BODYY+2,  "GAMES PLAYED", (snprintf(buf,32,"%d",played),buf),  C_FG);
            statRow(130, BODYY+2,  "AVG QUESTIONS",(snprintf(buf,32,"%d",avgQ),buf),    C_TITFG);
            M5Cardputer.Display.drawFastHLine(4,BODYY+30,DW-8,C_DIM);
            statRow(4,   BODYY+34, "COMPUTER WINS",(snprintf(buf,32,"%d",cW),buf),      C_ERR);
            statRow(130, BODYY+34, "YOUR WINS",    (snprintf(buf,32,"%d",pW),buf),      C_OK);
            M5Cardputer.Display.setTextSize(1);
            M5Cardputer.Display.setTextColor(C_DIM,C_BG);
            M5Cardputer.Display.setCursor(4,BODYY+62);
            M5Cardputer.Display.printf("Computer accuracy: %d",pct);
            M5Cardputer.Display.print("%");
            int bw=(int)((float)pct/100.f*(DW-8));
            uint16_t bc=pct>70?C_ERR:pct>40?C_WARN:C_OK;
            M5Cardputer.Display.fillRect(4,BODYY+71,bw,5,bc);
            M5Cardputer.Display.drawRect(4,BODYY+71,DW-8,5,C_DIM);

        } else if(page==1){
            // Streaks
            int cs=g_stats.currentStreak;
            if(cs>0)      snprintf(buf,32,"Computer x%d",cs);
            else if(cs<0) snprintf(buf,32,"You x%d",-cs);
            else          snprintf(buf,32,"None");
            statRow(4,BODYY+2,"CURRENT STREAK",buf,cs>0?C_ERR:cs<0?C_OK:C_DIM);
            M5Cardputer.Display.drawFastHLine(4,BODYY+30,DW-8,C_DIM);
            statRow(4,   BODYY+34,"BEST COMP STREAK", (snprintf(buf,32,"%d",g_stats.bestStreak),buf),       C_ERR);
            statRow(130, BODYY+34,"BEST YOUR STREAK", (snprintf(buf,32,"%d",g_stats.bestPlayerStreak),buf), C_OK);
            M5Cardputer.Display.drawFastHLine(4,BODYY+62,DW-8,C_DIM);
            statRow(4,BODYY+66,"TOTAL QUESTIONS",(snprintf(buf,32,"%d",g_stats.totalQuestions),buf),C_TITFG);

        } else {
            // Database + last word
            statRow(4,BODYY+2,"WORD DATABASE",(snprintf(buf,32,"%d words",g_wordCnt),buf),C_TITFG);
            M5Cardputer.Display.drawFastHLine(4,BODYY+30,DW-8,C_DIM);
            M5Cardputer.Display.setTextSize(1);
            M5Cardputer.Display.setTextColor(C_DIM,C_BG);
            M5Cardputer.Display.setCursor(4,BODYY+34);
            M5Cardputer.Display.print("LAST WORD TAUGHT");
            M5Cardputer.Display.setTextSize(2);
            M5Cardputer.Display.setTextColor(C_WARN,C_BG);
            M5Cardputer.Display.setCursor(4,BODYY+43);
            M5Cardputer.Display.print(g_stats.lastWord[0]?g_stats.lastWord:"(none yet)");
        }

        char c=waitCh();
        if(c==KLEFT  && page>0)       page--;
        else if(c==KRIGHT&&page<PAGES-1) page++;
        else if(c=='\b'||c=='\r')     return;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  HOW TO PLAY  -  4 pages, left/right to navigate, Backspace to exit
// ═══════════════════════════════════════════════════════════════════════════════
void howToPlay(){
    const int PAGES=4;
    int page=0;

    // Page content: title + lines (max 5 lines at textSize 2)
    struct HelpPage {
        const char* title;
        struct { const char* text; uint16_t col; } lines[6];
    };

    static const HelpPage HELP[PAGES] = {
        {
            "Keys",
            {
                { "^ v < >  = move",   C_TITFG },
                { "Enter    = select",  C_OK    },
                { "Bksp     = back",    C_DIM   },
                { "",                  C_BG    },
                { "< > also change",   C_DIM   },
                { "values in settings", C_DIM   },
            }
        },{
            "How it works",
            {
                { "Think of any word.",  C_FG    },
                { "I ask 20 questions.", C_FG    },
                { "Then I guess it.",    C_FG    },
                { "",                   C_BG    },
                { "First: pick a type", C_DIM   },
                { "from the grid.",     C_DIM   },
            }
        },{
            "Answering",
            {
                { "Page 1: common",     C_TITFG },
                { "Yes/No/Unknown...",  C_FG    },
                { "Page 2: nuanced",    C_TITFG },
                { "Maybe/Rarely...",    C_FG    },
                { "Down past row 2",    C_DIM   },
                { "flips the page.",    C_DIM   },
            }
        },{
            "Learning",
            {
                { "When I'm wrong,",    C_FG    },
                { "teach me the word.", C_FG    },
                { "I'll remember it",   C_FG    },
                { "next time.",         C_FG    },
                { "More games =",       C_DIM   },
                { "smarter guesses!",   C_OK    },
            }
        }
    };

    while(true){
        M5Cardputer.Display.fillScreen(C_BG);
        titleBar(HELP[page].title);
        pageDots(page,PAGES);
        M5Cardputer.Display.setTextSize(2);
        for(int i=0;i<6;i++){
            if(!HELP[page].lines[i].text[0]) continue;
            M5Cardputer.Display.setTextColor(HELP[page].lines[i].col,C_BG);
            M5Cardputer.Display.setCursor(4, BODYY + i*LH);
            M5Cardputer.Display.print(HELP[page].lines[i].text);
        }
        char c=waitCh();
        if(c==KLEFT  && page>0)          page--;
        else if(c==KRIGHT&&page<PAGES-1) page++;
        else if(c=='\b'||c=='\r')        return;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  MAIN MENU
// ═══════════════════════════════════════════════════════════════════════════════
void runMenu(){
    while(true){
        LItem items[4];
        strncpy(items[0].label,"Play",       51);items[0].lc=C_OK;   items[0].dot=C_OK;
        strncpy(items[1].label,"How to play",51);items[1].lc=C_TITFG;items[1].dot=0;
        strncpy(items[2].label,"Statistics", 51);items[2].lc=C_TITFG;items[2].dot=0;
        strncpy(items[3].label,"Settings",   51);items[3].lc=C_DIM;  items[3].dot=0;
        int ch=runList(items,4,"20 Questions");
        if(ch==0)      runGame();
        else if(ch==1) howToPlay();
        else if(ch==2) statsScreen();
        else if(ch==3) settingsMenu();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  BOOT ANIMATION
// ═══════════════════════════════════════════════════════════════════════════════
static void bootAnimation(){
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);

    const int cx = DW / 2;       // center x
    const int cy = 52;            // center y of the brain circle
    const int R  = 30;            // main radius

    for(int r = 2; r <= R; r += 2){
        d.drawCircle(cx, cy, r, C_TITBG);
        if(r > 4) d.drawCircle(cx, cy, r - 2, 0x0015u);  // trail
        delay(15);
    }
    d.fillCircle(cx, cy, R, 0x0015u);       // dark blue fill
    d.drawCircle(cx, cy, R, C_TITFG);       // cyan border
    d.drawCircle(cx, cy, R - 1, C_TITFG);

    d.setTextSize(1); d.setTextColor(C_TITFG, 0x0015u);
    d.setCursor(cx - 6, cy - 3); d.print("20");
    delay(100);
    d.fillCircle(cx, cy, R - 2, 0x0015u);
    d.setTextSize(3); d.setTextColor(C_TITFG, 0x0015u);
    d.setCursor(cx - 17, cy - 11); d.print("20");
    delay(100);

    const char* qm = "?";
    for(int i = 0; i < 20; i++){
        float angle = (float)i / 20.f * 6.2832f - 1.5708f;  // start from top
        int qx = cx + (int)((R + 14) * cosf(angle)) - 3;
        int qy = cy + (int)((R + 14) * sinf(angle)) - 4;

        if(i >= 2){
            float pa = (float)(i - 2) / 20.f * 6.2832f - 1.5708f;
            int px = cx + (int)((R + 14) * cosf(pa)) - 3;
            int py = cy + (int)((R + 14) * sinf(pa)) - 4;
            d.setTextSize(1); d.setTextColor(C_BG, C_BG);
            d.setCursor(px, py); d.print(qm);
        }

        uint16_t qcol = (i < 7) ? C_OK : (i < 14) ? C_WARN : C_ERR;
        d.setTextSize(1); d.setTextColor(qcol, C_BG);
        d.setCursor(qx, qy); d.print(qm);
        delay(40);
    }
    for(int i = 18; i < 22; i++){
        float pa = (float)(i % 20) / 20.f * 6.2832f - 1.5708f;
        int px = cx + (int)((R + 14) * cosf(pa)) - 3;
        int py = cy + (int)((R + 14) * sinf(pa)) - 4;
        d.setTextSize(1); d.setTextColor(C_BG, C_BG);
        d.setCursor(px, py); d.print(qm);
        delay(40);
    }

    d.fillCircle(cx, cy, R - 2, 0x0015u);
    d.setTextSize(3); d.setTextColor(C_WARN, 0x0015u);
    d.setCursor(cx - 25, cy - 11); d.print("20Q");
    d.drawCircle(cx, cy, R, C_WARN);
    d.drawCircle(cx, cy, R - 1, C_WARN);
    d.drawCircle(cx, cy, R + 1, C_WARN);
    delay(150);
    d.drawCircle(cx, cy, R + 1, C_BG);
    d.drawCircle(cx, cy, R, C_TITFG);
    d.drawCircle(cx, cy, R - 1, C_TITFG);

    const char* title = "20 Questions";
    d.setTextSize(2); d.setTextColor(C_TITFG, C_BG);
    int tx = (DW - (int)strlen(title) * 12) / 2;
    for(int i = 0; title[i]; i++){
        d.setCursor(tx + i * 12, cy + R + 10);
        d.print(title[i]);
        delay(45);
    }
    delay(100);

    const char* sub = "for M5Cardputer";
    d.setTextSize(1); d.setTextColor(C_DIM, C_BG);
    d.setCursor((DW - (int)strlen(sub) * 6) / 2, cy + R + 30);
    d.print(sub);
    delay(400);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SETUP / LOOP
// ═══════════════════════════════════════════════════════════════════════════════
void setup(){
    auto cfg=M5.config();
    M5Cardputer.begin(cfg,true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(C_BG);
    M5Cardputer.Display.setBrightness(128);
    Serial.begin(115200);
    g_lastInput=millis();

    bootAnimation();

    randomSeed(analogRead(0)^millis());

    // Allocate chunk buffer - 45KB, always fits in internal heap
    g_chunk=(int8_t*)malloc((size_t)CHUNK_SIZE*MAX_QUESTIONS);
    if(!g_chunk){
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setTextColor(C_ERR,C_BG);
        M5Cardputer.Display.setCursor(4, DH - 12); M5Cardputer.Display.print("malloc failed!");
        while(true) delay(1000);
    }
    memset(g_chunk,0,(size_t)CHUNK_SIZE*MAX_QUESTIONS);

    bool sdOk=SD.begin(M5.getPin(m5::pin_name_t::sd_spi_ss));
    if(sdOk){
        if(!SD.exists("/TwentyQ")) SD.mkdir("/TwentyQ");
        loadSettings();
        loadStats();
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setTextColor(C_DIM,C_BG);
        M5Cardputer.Display.setCursor(4, DH - 22); M5Cardputer.Display.print("Loading...");
        if(loadAll()){
            File wf=SD.open("/TwentyQ/weights.bin");
            size_t wfSize=wf?wf.size():0; if(wf)wf.close();
            size_t wfExpected=WEIGHT_HDR+(size_t)g_wordCnt*g_qCnt;

            M5Cardputer.Display.fillRect(0, DH - 22, DW, 22, C_BG);
            M5Cardputer.Display.setTextSize(1);
            M5Cardputer.Display.setTextColor(C_OK,C_BG);
            M5Cardputer.Display.setCursor(4, DH - 12);
            M5Cardputer.Display.printf("%d words, %d questions",g_wordCnt,g_qCnt);

            if(wfSize!=wfExpected){
                M5Cardputer.Display.fillScreen(C_BG);
                M5Cardputer.Display.setTextSize(2);
                M5Cardputer.Display.setTextColor(C_ERR,C_BG);
                M5Cardputer.Display.setCursor(4, 20);
                M5Cardputer.Display.printf("weights.bin: %u B",wfSize);
                M5Cardputer.Display.setCursor(4, 44);
                M5Cardputer.Display.printf("expected:    %u B",wfExpected);
                M5Cardputer.Display.setCursor(4, 72);
                M5Cardputer.Display.print("FILE CORRUPT!");
                M5Cardputer.Display.setTextSize(1);
                M5Cardputer.Display.setTextColor(C_DIM,C_BG);
                M5Cardputer.Display.setCursor(4, 100);
                M5Cardputer.Display.print("Recopy weights.bin to SD card.");
                delay(5000);
            }
        } else {
            M5Cardputer.Display.fillRect(0, DH - 22, DW, 22, C_BG);
            M5Cardputer.Display.setTextSize(1);
            M5Cardputer.Display.setTextColor(C_ERR,C_BG);
            M5Cardputer.Display.setCursor(4, DH - 22); M5Cardputer.Display.print("Missing CSV files!");
            M5Cardputer.Display.setTextColor(C_DIM,C_BG);
            M5Cardputer.Display.setCursor(4, DH - 12); M5Cardputer.Display.print("Need: words.csv + questions.csv");
        }
    } else {
        M5Cardputer.Display.fillRect(0, DH - 12, DW, 12, C_BG);
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setTextColor(C_ERR,C_BG);
        M5Cardputer.Display.setCursor(4, DH - 12); M5Cardputer.Display.print("SD card mount failed!");
    }
    delay(1500);
    runMenu();
}
void loop(){ vTaskDelay(50/portTICK_PERIOD_MS); }
