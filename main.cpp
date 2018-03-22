#include "mbed.h"
#include "SHA256.h"
#include "Timer.h"
#include "rtos.h"

//Photointerrupter input pins
#define I1pin D2
#define I2pin D11
#define I3pin D12

//Incremental encoder input pins
#define CHA   D7
#define CHB   D8  

//Motor Drive output pins   //Mask in output byte
#define L1Lpin D4           //0x01
#define L1Hpin D5           //0x02
#define L2Lpin D3           //0x04
#define L2Hpin D6           //0x08
#define L3Lpin D9           //0x10
#define L3Hpin D10          //0x20

//Mapping from sequential drive states to motor phase outputs
/*
State   L1  L2  L3
0       H   -   L
1       -   H   L
2       L   H   -
3       L   -   H
4       -   L   H
5       H   L   -
6       -   -   -
7       -   -   -
*/

//Drive state to output table
const int8_t driveTable[] = {0x12,0x18,0x09,0x21,0x24,0x06,0x00,0x00};

//Mapping from interrupter inputs to sequential rotor states. 0x00 and 0x07 are not valid
const int8_t stateMap[] = {0x07,0x05,0x03,0x04,0x01,0x00,0x02,0x07};  
//const int8_t stateMap[] = {0x07,0x01,0x03,0x02,0x05,0x00,0x04,0x07}; //Alternative if phase order of input or drive is reversed

//Status LED
DigitalOut led1(LED1);

//Photointerrupter inputs
InterruptIn I1(I1pin);
InterruptIn I2(I2pin);
InterruptIn I3(I3pin);

//Motor Drive outputs, L*L versions defined as PWM
PwmOut L1L(L1Lpin);
DigitalOut L1H(L1Hpin);
PwmOut L2L(L2Lpin);
DigitalOut L2H(L2Hpin);
PwmOut L3L(L3Lpin);
DigitalOut L3H(L3Hpin);

// Threads
Thread motorCtrlT(osPriorityNormal, 1024);  // Thread to control the speed and position of the motor
Thread commOutT(osPriorityNormal, 1024);    // Thread used for outgoing communications to serial
Thread commDec(osPriorityNormal, 2048);     // Thread to decode incoming regex instructions


// Function prototypes and global variables


//Function prototypes and global variables
void photoInterrupt();                              // Function which handles the photo interrupts

int8_t motorHome();                                 // Function to synchronise the motor's rotor 
int8_t orState = 0;                                 // Var. to store the rotor pos.  

void motorCtrlFn();                                 // Main function in motor control thread
void setPWMperiod(float period);                    // Function to set the PWM period for all pins
inline int8_t readRotorState();                     // Function to get the current rotor pos.
void motorOut(int8_t driveState, uint32_t torque);  // Function to drive the motor
void motorCtrlTick();                               // Function for periodic execution of the control thread
int32_t motorPosition = 0;                          // Current motor position
int32_t distanceToTarget = 0;                       // Distance to reach desired position
Mutex distanceToTarget_mutex;                       // Mutex for distanceToTarget
int32_t setVel = 0;                                 // Desired spin speed
Mutex setVel_mutex;                                 // Mutex for setVel
int8_t lead = 2;                                    // Spin direction: 2 for forwards, -2 for backwards
float PWMPeriod = 2000;                             // PWM period
float motorPower = 0;                               // Instantaneous desired motor power (pulse width)
int kp = 35;                                        // Controller var
int kd = 20;                                        // Controller var

void commOutFn();                                   // Main function in serial communication thread
void putMessage(uint8_t code, uint32_t data);       // Output message to serial comm
typedef struct{                                     // Struct used to send data to serial
    uint8_t code;
    uint32_t data;
} message_t ;
Mail<message_t,16> outMessages;                     // Mail to send data to serial from any thread
RawSerial pc(SERIAL_TX, SERIAL_RX);                 // Serial instantiation

void commDecFn();                                   // Main function in command decoding thread
Queue<void, 8> inCharQ;                             // Queue for incomming characters
void decodeV(char data[], int size);                // Decode velocity messages
void decodeR(char data[], int size);                // Decode rotation messages
void decodeK(char data[], int size);                // Decode bitcoin key messages

void mine();                                        // Function for bitcoin mining
volatile uint64_t newKey = 0;                       // Desired mining key
Mutex newKey_mutex;                                 // Mutex for newKey

int sgn(int val);                                   // Get the sign of an integer, returns +/-1
float max(float a, float b);                        // Return the maximum of two floats
float min(float a, float b);                        // Return the minimum of two floats

// Main loop************************************************************************************
int main() {

    // Start threads
    commOutT.start(commOutFn);              // Communication with the terminal thread
    commDec.start(commDecFn);               // Commands decoding thread
    
    // Set the desired PWM period
    setPWMperiod(PWMPeriod);

    // Synchronisation routine
    orState = motorHome();

    // Set the interrupts to the corresponding pins
    I1.rise(&photoInterrupt);
    I1.fall(&photoInterrupt);
    I2.rise(&photoInterrupt);
    I2.fall(&photoInterrupt);
    I3.rise(&photoInterrupt);
    I3.fall(&photoInterrupt);

    // Start motor control thread;
    motorCtrlT.start(motorCtrlFn);

    // Send ready code
    putMessage(99, 1);
    
    // Start mining
    mine();
}

// Interrupt routine
void photoInterrupt(){
    static int8_t oldRotorState;                // Previous rotor state
    int8_t rotorState = readRotorState();       // Updated rotor state
    
    // Update the motor pwm's according to the motor power
    motorOut((rotorState-orState+lead+6)%6, motorPower);

    // Update motorPosition according to the specific disk rotation direction
    if(rotorState - oldRotorState == 5) motorPosition--;
    else if(rotorState - oldRotorState == -5) motorPosition++;
    else motorPosition += (rotorState - oldRotorState);

    // Save the actual rotor state
    oldRotorState = rotorState;
}

// Motor control functions ****************************************************************
void motorCtrlFn(){
    volatile int32_t desiredVel = 0;                    // Target speed
    float measuredVel = 0;                              // Measured velocity
    float nextMotorPowerS = 0;                          // Motor power as required by the speed controller
    float nextMotorPowerR = 0;                          // Motor power as required by the position controller
    float nextMotorPower;                               // Final motor power that will be used
    Ticker motorCtrlTicker;                             // Ticker to control the execution of while(1)
    motorCtrlTicker.attach_us(&motorCtrlTick,100000);   // Interrupt to be triggered every 0.1 s
    Timer motorTimer;                                   // Timer to calculate the motor speed
    Timer motorTimer2;                                  // Timer to calculate the position rate of change
    int8_t timeReportCounter = 0;                       // Counter to report speed every x counts
    int oldDistanceToTarget = distanceToTarget;         // Used to compute the rate of change of the position
    int32_t motorPositionTemp;                          // Localy store motorPosition (thread safe)

    // Start the timers
    motorTimer2.start();
    motorTimer.start();

    // Start the main loop
    while(1){

        // Wait for ticker interrupt
        motorCtrlT.signal_wait(0x1);

        // Store target speed locally
        setVel_mutex.lock();
        desiredVel = setVel;
        setVel_mutex.unlock();

        // Get the current motor position
        motorPositionTemp = motorPosition;

        // Calculate current motor speed (times 6)
        measuredVel = motorPositionTemp/motorTimer.read();
        motorTimer.reset();

        // If the motor is not spining, and the distance to the target is non zero, start themotor
        if(measuredVel == 0 && abs(distanceToTarget)>0){
            photoInterrupt();
        }

        // Update error
        distanceToTarget -= motorPositionTemp;
        //putMessage(4, distanceToTarget);
        //putMessage(5, distanceToTarget);

        nextMotorPowerS = kp*(desiredVel - fabs(measuredVel))*sgn(distanceToTarget);
        nextMotorPowerR = kp*distanceToTarget+kd*(distanceToTarget-oldDistanceToTarget)/motorTimer2.read();
        motorTimer2.reset();
        oldDistanceToTarget = distanceToTarget;

        if(distanceToTarget<0){
            nextMotorPower = max(nextMotorPowerR, nextMotorPowerS);
        } else {
            nextMotorPower = min(nextMotorPowerR, nextMotorPowerS);
        }

        if(fabs(distanceToTarget) < 6){
            nextMotorPower = 0;
            distanceToTarget = 0;
            
        }


        if(nextMotorPower < 0){
            lead = -2;
            nextMotorPower = fabs(nextMotorPower);
        } else {
            lead = 2;
        }
        if(fabs(nextMotorPower)>1000){
            nextMotorPower=1000;
        }
        motorPower = nextMotorPower;
        
        motorPosition = 0;
        
        if(timeReportCounter++ == 19){
            if(measuredVel != 0 ){
                putMessage(10, measuredVel/6);
            }
            
            //putMessage(10, distanceToTarget);
            timeReportCounter = 0;
        }
        
        //

    }
}

//Basic synchronisation routine
int8_t motorHome() {
    //Put the motor in drive state 0 and wait for it to stabilise
    motorOut(0, PWMPeriod);
    wait(1.0);
    
    //Get the rotor state
    return readRotorState();
}

//Set a given drive state
void motorOut(int8_t driveState, uint32_t torque){
    
    //Lookup the output byte from the drive state.
    int8_t driveOut = driveTable[driveState & 0x07];
      
    //Turn off first
    if (~driveOut & 0x01) L1L.pulsewidth_us(0);
    if (~driveOut & 0x02) L1H = 1;
    if (~driveOut & 0x04) L2L.pulsewidth_us(0);
    if (~driveOut & 0x08) L2H = 1;
    if (~driveOut & 0x10) L3L.pulsewidth_us(0);
    if (~driveOut & 0x20) L3H = 1;
    
    //Then turn on
    if (driveOut & 0x01) L1L.pulsewidth_us(torque);
    if (driveOut & 0x02) L1H = 0;
    if (driveOut & 0x04) L2L.pulsewidth_us(torque);
    if (driveOut & 0x08) L2H = 0;
    if (driveOut & 0x10) L3L.pulsewidth_us(torque);
    if (driveOut & 0x20) L3H = 0;
}

void setPWMperiod(float period){
    L1L.period_us(period);
    L2L.period_us(period);
    L3L.period_us(period);
}

void motorCtrlTick(){motorCtrlT.signal_set(0x1);}

inline int8_t readRotorState(){
    return stateMap[I1 + 2*I2 + 4*I3];
}

void commOutFn(){
    while(1) {
        osEvent newEvent = outMessages.get();
        message_t *pMessage = (message_t*)newEvent.value.p;
        pc.printf("Message %d with data 0x%016x\r\n",
        pMessage->code,pMessage->data);
        outMessages.free(pMessage);
    }
}

void putMessage(uint8_t code, uint32_t data){
    message_t *pMessage = outMessages.alloc();
    pMessage->code = code;
    pMessage->data = data;
    outMessages.put(pMessage);
 }

void serialISR(){
    uint8_t newChar = pc.getc();
    inCharQ.put((void*)newChar);
}

// Serial commands decoding functions******************************************
void commDecFn(){
    pc.attach(&serialISR);

    // inCommPtr stores the next empty index in inComm array (stores incomming message from serial)
    int inCommPtr = 0;
    const int textBufferSize = 30;
    char inComm[textBufferSize];

    while(1){
            osEvent newEvent = inCharQ.get();
            uint8_t newChar = (uint8_t) newEvent.value.p;
        if(newChar != (char)'\r'){
            inComm[inCommPtr++] = newChar;
        }else{
            inComm[inCommPtr++] = (char)'\0';
            inCommPtr = 0;
            // Decode command
            if(inComm[0] == (char)'R') decodeR(inComm, textBufferSize);
            if(inComm[0] == (char)'V') decodeV(inComm, textBufferSize);
            if(inComm[0] == (char)'K') decodeK(inComm, textBufferSize);//decodeK();
        }
        if(inCommPtr == 30){
            inCommPtr = 0;
        }   
    }
}

void decodeV(char data[], int size){
    //Decode velocity
    int32_t newVel;
    sscanf(data, "V%u", &newVel);
    newVel = newVel*6;
    if(newVel == 0){
        newVel = 2000;
    }
    //Send the new velocity to motor control thread
    setVel_mutex.lock();
    setVel = newVel;
    setVel_mutex.unlock();
    /*int32_t *velMessage = newVel_Mail.alloc();
    *velMessage = newVel;
    newVel_Mail.put(velMessage);*/
}

void decodeR(char data[], int size){
    //Decode velocity
    int32_t newTarget;
    sscanf(data, "R%u", &newTarget);
    if(newTarget != 0){
        newTarget = newTarget*6 + orState;
    } else {
        newTarget = 2147483647;
    }
    
    putMessage(6, newTarget);
    distanceToTarget_mutex.lock();
    distanceToTarget = (int32_t) newTarget;
    distanceToTarget_mutex.unlock();
}

void decodeK(char data[], int size){
     
     uint64_t tempInt1 = 0;
     uint32_t  tempInt2 = 0;
     //uint64_t finalKey = 0;
     //sscanf(inComm, "K%llx", &newKey);
     /*uint8_t* tempPtr = (uint8_t*) &newKey;
     
     for(int i = 0; i < 16; i = i + 2){
         
         sscanf(&data[16-i], "%1x", &tempInt1);
         sscanf(&data[15-i], "%1x", &tempInt2);
         *tempPtr = (uint8_t)(tempInt2 << 4 | tempInt1);
         tempPtr++;
     }*/
     sscanf(data, "K%8x", &tempInt1);
     sscanf(data+9, "%8x", &tempInt2);
     //sscanf(data+36, "%8x", &newKey+32);
     //pc.printf("%16x", tempInt1);
     newKey_mutex.lock();
     newKey = tempInt1 << 32 | tempInt2;
     newKey_mutex.unlock();
     //putMessage(76, tempInt1);
     //putMessage(76, tempInt2);
    //pc.printf("%16x", finalKey);
     //newKey_mutex.lock();
     //newKey = (uint64_t)(tempInt1 << 32 | tempInt2);
     //newKey_mutex.unlock();
     //putMessage(76, newKey);
 }


int sgn(int val){
    if(val>0){
        return 1;
    } else if(val<0) {
        return -1;
    } else {
        return 0;
    }
}

float max(float a, float b){
    if(a < b){
        return b;
    } else {
        return a;
    }
}

float min(float a, float b){
    if(a > b){
        return b;
    } else {
        return a;
    }
}

void mine(){
    //Crypto
    SHA256 crypt;
    //rial pc2(SERIAL_TX, SERIAL_RX);
    uint8_t sequence[] = {0x45,0x6D,0x62,0x65,0x64,0x64,0x65,0x64,
    0x20,0x53,0x79,0x73,0x74,0x65,0x6D,0x73,
    0x20,0x61,0x72,0x65,0x20,0x66,0x75,0x6E,
    0x20,0x61,0x6E,0x64,0x20,0x64,0x6F,0x20,
    0x61,0x77,0x65,0x73,0x6F,0x6D,0x65,0x20,
    0x74,0x68,0x69,0x6E,0x67,0x73,0x21,0x20,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    uint64_t* key = (uint64_t*)((int)sequence + 48);
    uint64_t* nonce = (uint64_t*)((int)sequence + 56);
    uint8_t hash[32];
    
    uint8_t hashCount = 0;
    Timer t;
    

    while (1) {
        t.start();
        //putMessage(90, 1);
        newKey_mutex.lock();
        *key = newKey;
        newKey_mutex.unlock();
        //*key = 0;
        crypt.computeHash(hash, sequence, 64);

        *nonce = *nonce + 1;
        //pc.printf("%d\n\r", nonce);
        if(hash[0] == 0 && hash[1] == 0){
            putMessage(90, *key);
            putMessage(91, *nonce);
            hashCount++;
        }
        //t.stop();
        // if(t.read() > 1){
        //     t.reset();
        //     pc2.printf("%d\n\r", hashCount);
        //     hashCount = 0;
        // }
        //t.start()
    }
}