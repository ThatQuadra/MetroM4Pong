//Notes:
//Brightness ranges from 384 to 1280 (896 levels)
//500-1000 is most usable (outside that might cause sync issues and artifacting)
//Actual usable area on the screen has been about (1≤x≤45, 1≤y≤24)

#include "pixelart.h"

//Total NTSC lines, visible lines, effective lines to generate (in order to achieve a decent aspect ratio), 
//total number of "columns" (nanoseconds per line, as each DAC clock step is a nanosecond), and visible "columns", respectively
#define TOTLINES 525
#define VIEWLINES 243
#define VIDLINES 27
#define TOTCOLUMNS 64
#define VIDCOLUMNS 52

// Defining the arrays of points for what's given to the DMA controller (data) and the framebuffer (buffer)
uint16_t data[TOTLINES*TOTCOLUMNS];
uint16_t buffer[VIDLINES*VIDCOLUMNS];

int i;
int l;
int c;
int t;
int a;
int val;
int timestep; //number of main cycles before running the gametick function
int ballx;
int bally;
int ballvx;
int ballvy;
int paddleya;
int paddleyb;
int scorea;
int scoreb;
int maxscore = 12;
int gamestate = 0; //0: start screen, 1: gameplay, 2: player 1 win, 3: player 2 win
uint8_t btnlast; //previous state of button, to detect falling edge
static DmacDescriptor descriptor1 __attribute__((aligned(16)));

// DMA functions, borrowed and modified from the code here: https://forum.arduino.cc/t/samd51-dac-using-dma-seems-too-fast/678418

//Set up DMA control
void dma_init() {

  static DmacDescriptor descriptor __attribute__((aligned(16)));
  static DmacDescriptor descriptor1 __attribute__((aligned(16)));
  static DmacDescriptor wrb __attribute__((aligned(16)));
  static uint32_t chnl0 = 0;  // DMA channel
  DMAC->BASEADDR.reg = (uint32_t)&descriptor1;
  DMAC->WRBADDR.reg = (uint32_t)&wrb;
  DMAC->CTRL.bit.LVLEN0 = 1 ;
  DMAC->CTRL.bit.LVLEN1 = 1 ;
  DMAC->CTRL.bit.LVLEN2 = 1 ;
  DMAC->CTRL.bit.LVLEN3 = 1 ;
  DMAC->CTRL.bit.DMAENABLE = 1;
  DMAC->Channel[0].CHCTRLA.reg = DMAC_CHCTRLA_TRIGSRC(0x49) |   // DAC DATA trigger
                                 DMAC_CHCTRLA_TRIGACT_BURST;    // Burst transfers
  descriptor1.DESCADDR.reg = (uint32_t) &descriptor1;
  descriptor1.BTCTRL.bit.VALID    = 0x1; //Its a valid channel
  descriptor1.BTCTRL.bit.BEATSIZE = 0x1;  // HWORD.
  descriptor1.BTCTRL.bit.SRCINC   = 0x1;   //Source increment is enabled
  descriptor1.BTCTRL.bit.DSTINC   = 0x0;   //Destination increment disabled
  descriptor1.BTCTRL.bit.BLOCKACT = 0x2;   //Suspend after block complete.
  // ("Burst" size will be 1 "beat" = 1 HWORD, by default)
  descriptor1.BTCNT.reg           = TOTLINES*TOTCOLUMNS;   // points to send
  descriptor1.SRCADDR.reg         = (uint32_t)(&data[TOTLINES*TOTCOLUMNS]); //send from the data vevtor
  descriptor1.DSTADDR.reg         = (uint32_t)&DAC->DATA[1].reg;   //to the DAC output
  // start channel
  DMAC ->Channel[0].CHCTRLA.bit.ENABLE = 0x1;     //OK
  DMAC->CTRL.bit.DMAENABLE = 1;
}

//Setup DAC
void dac_init() {
 
 GCLK->GENCTRL[7].reg = GCLK_GENCTRL_DIV(4) |       // Divide the 48MHz clock source by divisor 4: 48MHz/4 = 12MHz
                         GCLK_GENCTRL_GENEN |        // Enable GCLK7
                         GCLK_GENCTRL_SRC_DFLL;      // Select 48MHz DFLL clock source

  while (GCLK->SYNCBUSY.bit.GENCTRL7);
  GCLK->PCHCTRL[42].reg = GCLK_PCHCTRL_CHEN |        // Enable the DAC peripheral channel
                          GCLK_PCHCTRL_GEN_GCLK7;    // Connect generic clock 7 to DAC
  MCLK->APBDMASK.bit.DAC_ = 1;
  DAC->CTRLA.bit.SWRST = 1;
  while (DAC->CTRLA.bit.SWRST);
  DAC->DACCTRL[1].reg = DAC_DACCTRL_REFRESH(2) |
                        DAC_DACCTRL_CCTRL_CC12M |
                        DAC_DACCTRL_ENABLE
                        //                        | DAC_DACCTRL_FEXT
                        ;
  DAC_DACCTRL_LEFTADJ;
  DAC->CTRLA.reg = DAC_CTRLA_ENABLE;
  while (DAC->SYNCBUSY.bit.ENABLE);
  while (!DAC->STATUS.bit.READY1);
  PORT->Group[0].DIRSET.reg = (1 << 2);
  PORT->Group[0].PINCFG[5].bit.PMUXEN = 1;
  PORT->Group[0].PMUX[1].bit.PMUXE = 1;
}

//Push data from the buffer array into the data array to be drawn and account for differences in size
void buftodata(){
  for(l = 0; l < VIDLINES; l++){
    for (c = 0; c<VIDCOLUMNS; c++){
      val = buffer[(VIDCOLUMNS*l)+c];
      for (a = 0; a<9; a++){
        data[((9*l)+a)*TOTCOLUMNS+12+c] = val;
        data[(((9*l)+263+a)*TOTCOLUMNS)+12+c] = val;
      }
    }
  }
}

//Write a single point to the buffer with a supplied value
void point(int x, int y, int v){
  buffer[(y*VIDCOLUMNS)+x] = v;
}

//Set the buffer to a test pattern to confirm output, this draws a horizontal gradient
void testpattern(){
  for(l = 0; l < VIDLINES; l++){
    for (c = 0; c < VIDCOLUMNS; c++) {
      point(c,l, int((c*500)/VIDCOLUMNS)+500);
    }
  }
}

//Set buffer to all black
void blank(){
  for(l = 0; l < VIDLINES; l++){
    for (c = 0; c < VIDCOLUMNS; c++) {
      point(c,l,500);
    }
  }
}

//Reset game for new round
void resetgame(){
  ballx = 23;
  bally = 12;
  timestep = 60;

  //read analog input to produce better random values
  randomSeed(analogRead(A4));
  a = random(4);

  //Set ball initial direction
  switch (a){
    case 0:
      ballvx = 1;
      ballvy = 1;
      break;
    case 1:
      ballvx = 1;
      ballvy = -1;
      break;
    case 2:
      ballvx = -1;
      ballvy = 1;
      break;
    case 3:
      ballvx = -1;
      ballvy = -1;
      break;
  }
}

//Write game elements to the buffer
void drawgame(){
  //Draw ball
  point(ballx, bally, 1000);
  
  //Draw paddles
  paddleya = (analogRead(A2)/53)+3;
  for (l = paddleya-2; l < paddleya+3; l++){
    point(3,l,1000);
  }
  paddleyb = (analogRead(A3)/53)+3;
  for (l = paddleyb-2; l < paddleyb+3; l++){
    point(43,l,1000);
  }

  //Draw score bars
  for (l = 24; l > 24 - scorea; l--){
    point(1,l,1000);
  }
  for (l = 24; l > 24 - scoreb; l--){
    point(45,l,1000);
  }

  //Draw separator lines
  for (l = 0; l < 25; l++){
    point(2,l,650);
  }
  for (l = 0; l < 25; l++){
    point(44,l,650);
  }
}

//Tick game forward
void gametick(){
  //Vertical bounce
  if ((bally < 1)||(bally > 24)){
    ballvy = -ballvy;
    tone(4, 440, 100);
  }

  //Check if ball is all the way to left or right. Bounce if touching paddle, increase score and reset if not
  if ((ballx < 5)||(ballx > 41)){
    if (((ballx < 5)&&(abs(bally-paddleya) < 3))||((ballx > 41)&&(abs(bally-paddleyb) < 3))){ //Paddle check
      ballvx = -ballvx;
      if (timestep > 2){
        timestep -= 2; //Speed up game
      }
      tone(4, 523, 100);
    }

    //If not in contact with paddle
    else{ 
      if (ballx < 4){
        scoreb += 1;
        tone(4, 1760, 100);
        resetgame();
      }
      if (ballx > 42){
        scorea += 1;
        tone(4, 1760, 100);
        resetgame();
      }
    }
  }
  
  //Move ball
  ballx += ballvx;
  bally += ballvy;

  //Check for win conditions and set game state to a win screen if they're met
  if (scorea >= maxscore){
    gamestate = 2;
    tone(4, 659, 100);
  }
  else if (scoreb >= maxscore){
    gamestate = 3;
    tone(4, 659, 100);
  }
}

void setup() {
  //Setup data array, including vertical and horizontal sync
  for(l = 0; l < TOTLINES; l ++){
    if ((l > 490) && (l < 513)){
      //Horizontal sync
      for (c = 0; c < 4; c++) {
        data[(l*TOTCOLUMNS)+c] = 0;
      }
      for (c = 4; c < 12; c++) {
        data[(l*TOTCOLUMNS)+c] = 0;
      }

      //Empty framebuffer
      for (c = 12; c < 64; c++) {
        data[(l*TOTCOLUMNS)+c] = 0;
     }
    }
    else{
      //Vertical sync
      for (c = 0; c < 4; c++) {
        data[(l*TOTCOLUMNS)+c] = 0;
      }
    
      for (c = 4; c < 12; c++) {
        data[(l*TOTCOLUMNS)+c] = 480;
      }
      for (c = 12; c < 64; c++) {
        data[(l*TOTCOLUMNS)+c] = 480;
     }
    }
  }

  dac_init();
  dma_init();

  //Setup pin for reading button
  pinMode(11, INPUT_PULLUP);
  btnlast = digitalRead(11);

  tone(4, 1760, 100);
}


void loop() {
  DMAC ->Channel[0].CHCTRLB.reg = 0x2; //Run DMA Output

  buftodata();
  blank();
  
  //Start screen
  if (gamestate == 0){
    for(a = 0; a < VIDLINES*VIDCOLUMNS; a++ ){
      buffer[a] = start[a];
    }
  }

  //Gameplay
  if (gamestate == 1){
    drawgame();
    i++;
    if(i % timestep == timestep-1){
      gametick();
    t++;
    }
  }

  //Win screens
  if (gamestate == 2){
    for(a = 0; a < VIDLINES*VIDCOLUMNS; a++ ){
      buffer[a] = awin[a];
    }
  }
  if (gamestate == 3){
    for(a = 0; a < VIDLINES*VIDCOLUMNS; a++ ){
      buffer[a] = bwin[a];
    }
  }

  // Check button to change states
  if (digitalRead(11)==LOW && btnlast == HIGH){
    scorea = 0;
    scoreb = 0;
    if (gamestate == 0){
      resetgame();
      gamestate = 1;
    }
    else{
      gamestate = 0;
      tone(4, 1760, 100);
    }
  }
  btnlast = digitalRead(11);
}
