#ifndef REVERBTAIL_H_INCLUDED
#define REVERBTAIL_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"
#include "FilterBank.h"

#include <numeric>

#define FDN_ORDER 16 // speed of sound in m.s-1

class ReverbTail
{

//==========================================================================
// ATTRIBUTES
    
public:

    std::vector<float> valuesRT60; // in sec
    float initGain = 0.0f; // S.I.
    float initDelay = 0.0f; // in sec
    
private:

    // octave filter bank
    // FilterBank filterBank;
    int numOctaveBands = 3;
    // std::vector<IIRFilter> freqFilters;
    
    // local delay line
    DelayLine delayLine;
    // bool reverbTailReadyToUse;
    
    // setup FDN (static FDN order of 16 is max for now)
    std::array<float, 16> fdnDelays; // in sec
    // std::array<float, 16> fdnGains; // S.I.
    std::array< std::array < float, 16>, 3 > fdnGains; // S.I.
    std::array< std::array < float, 16>, 16 > fdnFeedbackMatrix; // S.I.
    
    // audio buffers
    AudioBuffer<float> reverbBusBuffers; // working buffer
    AudioBuffer<float> workingBuffer; // working buffer
    AudioBuffer<float> tailBuffer;

    // RT60 values
    // std::vector<float> slopesRT60;
    
    // misc.
    double localSampleRate;
    int localSamplesPerBlockExpected;
    // Array<float> absorbanceRT60;
    
//==========================================================================
// METHODS
    
public:
    
ReverbTail() {
    // set to NUM_OCTAVE_BANDS rather than local numOctaveBands since anyway get methods
    // from OSCHandler will resize them (for "clarity's" sake here).
//    slopesRT60.resize( NUM_OCTAVE_BANDS, 0.0f );
    
//    absorbanceRT60.resize( numOctaveBands );
    
    valuesRT60.resize( NUM_OCTAVE_BANDS, 0.0f );
    
    
    defineFdnFeedbackMatrix();
    updateFdnParameters();
    
    // tailNoises.resize( numOctaveBands );
    // reverbTailReadyToUse = false;
}

~ReverbTail() {}

// local equivalent of prepareToPlay
void prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    // prepare buffers
    reverbBusBuffers.setSize(FDN_ORDER*3, samplesPerBlockExpected); // for 3 freq bands
    reverbBusBuffers.clear();
    tailBuffer.setSize(FDN_ORDER, samplesPerBlockExpected);
    
    workingBuffer.setSize(1, samplesPerBlockExpected);
    workingBuffer.clear();
    
    // init delay line
    delayLine.prepareToPlay(samplesPerBlockExpected, sampleRate);
    delayLine.setSize(FDN_ORDER*3, sampleRate); // debug: set delay line max size
    
    // keep local copies
    localSampleRate = sampleRate;
    localSamplesPerBlockExpected = samplesPerBlockExpected;
}

void updateInternals( std::vector<float> r60Values, float newInitGain, float newInitDelay )
{
    // store new values
    initDelay = newInitDelay;
    initGain = newInitGain;
    valuesRT60 = r60Values;
    
    // increase fdn delay line length if need be
    float maxDelay = getMaxValue(valuesRT60);
    delayLine.setSize(FDN_ORDER*3, maxDelay*localSampleRate);
    
    // update FDN parameters
    updateFdnParameters();
}

// add source image to reverberation bus for latter use
void addToBus( int busId, AudioBuffer<float> source )
{
    // TODO: SUPPORT 10 CHANNELS INPUTS (i.e. downmix to 3 here)
    if( source.getNumChannels() == 3 )
    {
        for( int k = 0; k < source.getNumChannels(); k++ )
        {
            reverbBusBuffers.addFrom(k*FDN_ORDER+busId, 0, source, k, 0, localSamplesPerBlockExpected);
        }
    }
}
    
// process reverb tail from bus tail, return reverb tail buffer
AudioBuffer<float> getTailBuffer()
{
    // loop over FDN bus to write direct input to / read output from delay line
    float delayInFractionalSamples;
    tailBuffer.clear();
    workingBuffer.clear();
    
    int bufferIndex = 0;
    for (int fdnId = 0; fdnId < FDN_ORDER; fdnId++)
    {
        delayInFractionalSamples = fdnDelays[fdnId] * localSampleRate;
        
        for (int bandId = 0; bandId < 3; bandId++)
        {
            bufferIndex = bandId*FDN_ORDER + fdnId;
            
            // clear delay line head (copy cleared buffer): necessary, to recursively use the delayLine addFrom afterwards
            delayLine.copyFrom( bufferIndex, workingBuffer, 0, 0, localSamplesPerBlockExpected );
            
            // write input to delay line
            delayLine.addFrom( bufferIndex, reverbBusBuffers, bufferIndex, 0, localSamplesPerBlockExpected );
        
            // read output from delay line (erase current content of reverbBuffers)
            reverbBusBuffers.copyFrom(bufferIndex, 0,
                                      delayLine.getInterpolatedChunk(bufferIndex, localSamplesPerBlockExpected, delayInFractionalSamples),
                                      0, 0, localSamplesPerBlockExpected);
            
            // sum FDN to output
            tailBuffer.addFrom(fdnId, 0, reverbBusBuffers, bufferIndex, 0, localSamplesPerBlockExpected);
            
            // apply FDN gains
            reverbBusBuffers.applyGain(bufferIndex, 0, localSamplesPerBlockExpected, fdnGains[bandId][fdnId]);
        }
        
    }
    
    // write fdn outputs to delay lines (with cross-feedback matrix)
    // had to put this in a different loop to await for reverbBusBuffers fill
    for (int fdnId = 0; fdnId < FDN_ORDER; fdnId++)
    {
         for (int fdnFedId = 0; fdnFedId < FDN_ORDER; fdnFedId++)
        {
            for (int bandId = 0; bandId < 3; bandId++)
            {
                // get fdnFedId output, apply cross-feedback gain
                workingBuffer.copyFrom(0, 0, reverbBusBuffers, fdnFedId + bandId*FDN_ORDER, 0, localSamplesPerBlockExpected);
                workingBuffer.applyGain(fdnFeedbackMatrix[fdnId][fdnFedId]);
                
                // write to fdnId (delayLine)
                delayLine.addFrom(fdnId + bandId*FDN_ORDER, workingBuffer, 0, 0, localSamplesPerBlockExpected );
            }
        }
    }
    
    // increment delay line write position
    delayLine.incrementWritePosition(localSamplesPerBlockExpected);
    
    // clear reverb bus
    reverbBusBuffers.clear();
    
    return tailBuffer;


    
}

    
private:
  
    void updateFdnParameters(){
        
        // TO DO: delay values should be based on dist min / max (see pd patch / Sabine formula)
        fdnDelays[0] = 0.011995;
        fdnDelays[1] = 0.019070;
        fdnDelays[2] = 0.021791;
        fdnDelays[3] = 0.031043;
        fdnDelays[4] = 0.038118;
        fdnDelays[5] = 0.041927;
        fdnDelays[6] = 0.050091;
        fdnDelays[7] = 0.063696;
        fdnDelays[8] = 0.078934;
        fdnDelays[9] = 0.084376;
        fdnDelays[10] = 0.101791;
        fdnDelays[11] = 0.114308;
        fdnDelays[12] = 0.120839;
        fdnDelays[13] = 0.141519;
        fdnDelays[14] = 0.156213;
        fdnDelays[15] = 0.179615;
        
        for (int bandId = 0; bandId < 3; bandId++)
        {
            for (int fdnId = 0; fdnId < FDN_ORDER; fdnId++)
            {   // TODO: valuesRT60 are still for 10 bands, to be converted
                fdnGains[bandId][fdnId] = pow( 10, -3*fdnDelays[fdnId] / valuesRT60[bandId] );
            }
        }
    }

// direct copy of output of hadamar matrix as defined in reverbTailv3.m file
void defineFdnFeedbackMatrix(){
    fdnFeedbackMatrix[0][0] = 0.250;
    fdnFeedbackMatrix[0][1] = 0.250;
    fdnFeedbackMatrix[0][2] = 0.250;
    fdnFeedbackMatrix[0][3] = 0.250;
    fdnFeedbackMatrix[0][4] = 0.250;
    fdnFeedbackMatrix[0][5] = 0.250;
    fdnFeedbackMatrix[0][6] = 0.250;
    fdnFeedbackMatrix[0][7] = 0.250;
    fdnFeedbackMatrix[0][8] = 0.250;
    fdnFeedbackMatrix[0][9] = 0.250;
    fdnFeedbackMatrix[0][10] = 0.250;
    fdnFeedbackMatrix[0][11] = 0.250;
    fdnFeedbackMatrix[0][12] = 0.250;
    fdnFeedbackMatrix[0][13] = 0.250;
    fdnFeedbackMatrix[0][14] = 0.250;
    fdnFeedbackMatrix[0][15] = 0.250;
    fdnFeedbackMatrix[1][0] = 0.250;
    fdnFeedbackMatrix[1][1] = -0.250;
    fdnFeedbackMatrix[1][2] = 0.250;
    fdnFeedbackMatrix[1][3] = -0.250;
    fdnFeedbackMatrix[1][4] = 0.250;
    fdnFeedbackMatrix[1][5] = -0.250;
    fdnFeedbackMatrix[1][6] = 0.250;
    fdnFeedbackMatrix[1][7] = -0.250;
    fdnFeedbackMatrix[1][8] = 0.250;
    fdnFeedbackMatrix[1][9] = -0.250;
    fdnFeedbackMatrix[1][10] = 0.250;
    fdnFeedbackMatrix[1][11] = -0.250;
    fdnFeedbackMatrix[1][12] = 0.250;
    fdnFeedbackMatrix[1][13] = -0.250;
    fdnFeedbackMatrix[1][14] = 0.250;
    fdnFeedbackMatrix[1][15] = -0.250;
    fdnFeedbackMatrix[2][0] = 0.250;
    fdnFeedbackMatrix[2][1] = 0.250;
    fdnFeedbackMatrix[2][2] = -0.250;
    fdnFeedbackMatrix[2][3] = -0.250;
    fdnFeedbackMatrix[2][4] = 0.250;
    fdnFeedbackMatrix[2][5] = 0.250;
    fdnFeedbackMatrix[2][6] = -0.250;
    fdnFeedbackMatrix[2][7] = -0.250;
    fdnFeedbackMatrix[2][8] = 0.250;
    fdnFeedbackMatrix[2][9] = 0.250;
    fdnFeedbackMatrix[2][10] = -0.250;
    fdnFeedbackMatrix[2][11] = -0.250;
    fdnFeedbackMatrix[2][12] = 0.250;
    fdnFeedbackMatrix[2][13] = 0.250;
    fdnFeedbackMatrix[2][14] = -0.250;
    fdnFeedbackMatrix[2][15] = -0.250;
    fdnFeedbackMatrix[3][0] = 0.250;
    fdnFeedbackMatrix[3][1] = -0.250;
    fdnFeedbackMatrix[3][2] = -0.250;
    fdnFeedbackMatrix[3][3] = 0.250;
    fdnFeedbackMatrix[3][4] = 0.250;
    fdnFeedbackMatrix[3][5] = -0.250;
    fdnFeedbackMatrix[3][6] = -0.250;
    fdnFeedbackMatrix[3][7] = 0.250;
    fdnFeedbackMatrix[3][8] = 0.250;
    fdnFeedbackMatrix[3][9] = -0.250;
    fdnFeedbackMatrix[3][10] = -0.250;
    fdnFeedbackMatrix[3][11] = 0.250;
    fdnFeedbackMatrix[3][12] = 0.250;
    fdnFeedbackMatrix[3][13] = -0.250;
    fdnFeedbackMatrix[3][14] = -0.250;
    fdnFeedbackMatrix[3][15] = 0.250;
    fdnFeedbackMatrix[4][0] = 0.250;
    fdnFeedbackMatrix[4][1] = 0.250;
    fdnFeedbackMatrix[4][2] = 0.250;
    fdnFeedbackMatrix[4][3] = 0.250;
    fdnFeedbackMatrix[4][4] = -0.250;
    fdnFeedbackMatrix[4][5] = -0.250;
    fdnFeedbackMatrix[4][6] = -0.250;
    fdnFeedbackMatrix[4][7] = -0.250;
    fdnFeedbackMatrix[4][8] = 0.250;
    fdnFeedbackMatrix[4][9] = 0.250;
    fdnFeedbackMatrix[4][10] = 0.250;
    fdnFeedbackMatrix[4][11] = 0.250;
    fdnFeedbackMatrix[4][12] = -0.250;
    fdnFeedbackMatrix[4][13] = -0.250;
    fdnFeedbackMatrix[4][14] = -0.250;
    fdnFeedbackMatrix[4][15] = -0.250;
    fdnFeedbackMatrix[5][0] = 0.250;
    fdnFeedbackMatrix[5][1] = -0.250;
    fdnFeedbackMatrix[5][2] = 0.250;
    fdnFeedbackMatrix[5][3] = -0.250;
    fdnFeedbackMatrix[5][4] = -0.250;
    fdnFeedbackMatrix[5][5] = 0.250;
    fdnFeedbackMatrix[5][6] = -0.250;
    fdnFeedbackMatrix[5][7] = 0.250;
    fdnFeedbackMatrix[5][8] = 0.250;
    fdnFeedbackMatrix[5][9] = -0.250;
    fdnFeedbackMatrix[5][10] = 0.250;
    fdnFeedbackMatrix[5][11] = -0.250;
    fdnFeedbackMatrix[5][12] = -0.250;
    fdnFeedbackMatrix[5][13] = 0.250;
    fdnFeedbackMatrix[5][14] = -0.250;
    fdnFeedbackMatrix[5][15] = 0.250;
    fdnFeedbackMatrix[6][0] = 0.250;
    fdnFeedbackMatrix[6][1] = 0.250;
    fdnFeedbackMatrix[6][2] = -0.250;
    fdnFeedbackMatrix[6][3] = -0.250;
    fdnFeedbackMatrix[6][4] = -0.250;
    fdnFeedbackMatrix[6][5] = -0.250;
    fdnFeedbackMatrix[6][6] = 0.250;
    fdnFeedbackMatrix[6][7] = 0.250;
    fdnFeedbackMatrix[6][8] = 0.250;
    fdnFeedbackMatrix[6][9] = 0.250;
    fdnFeedbackMatrix[6][10] = -0.250;
    fdnFeedbackMatrix[6][11] = -0.250;
    fdnFeedbackMatrix[6][12] = -0.250;
    fdnFeedbackMatrix[6][13] = -0.250;
    fdnFeedbackMatrix[6][14] = 0.250;
    fdnFeedbackMatrix[6][15] = 0.250;
    fdnFeedbackMatrix[7][0] = 0.250;
    fdnFeedbackMatrix[7][1] = -0.250;
    fdnFeedbackMatrix[7][2] = -0.250;
    fdnFeedbackMatrix[7][3] = 0.250;
    fdnFeedbackMatrix[7][4] = -0.250;
    fdnFeedbackMatrix[7][5] = 0.250;
    fdnFeedbackMatrix[7][6] = 0.250;
    fdnFeedbackMatrix[7][7] = -0.250;
    fdnFeedbackMatrix[7][8] = 0.250;
    fdnFeedbackMatrix[7][9] = -0.250;
    fdnFeedbackMatrix[7][10] = -0.250;
    fdnFeedbackMatrix[7][11] = 0.250;
    fdnFeedbackMatrix[7][12] = -0.250;
    fdnFeedbackMatrix[7][13] = 0.250;
    fdnFeedbackMatrix[7][14] = 0.250;
    fdnFeedbackMatrix[7][15] = -0.250;
    fdnFeedbackMatrix[8][0] = 0.250;
    fdnFeedbackMatrix[8][1] = 0.250;
    fdnFeedbackMatrix[8][2] = 0.250;
    fdnFeedbackMatrix[8][3] = 0.250;
    fdnFeedbackMatrix[8][4] = 0.250;
    fdnFeedbackMatrix[8][5] = 0.250;
    fdnFeedbackMatrix[8][6] = 0.250;
    fdnFeedbackMatrix[8][7] = 0.250;
    fdnFeedbackMatrix[8][8] = -0.250;
    fdnFeedbackMatrix[8][9] = -0.250;
    fdnFeedbackMatrix[8][10] = -0.250;
    fdnFeedbackMatrix[8][11] = -0.250;
    fdnFeedbackMatrix[8][12] = -0.250;
    fdnFeedbackMatrix[8][13] = -0.250;
    fdnFeedbackMatrix[8][14] = -0.250;
    fdnFeedbackMatrix[8][15] = -0.250;
    fdnFeedbackMatrix[9][0] = 0.250;
    fdnFeedbackMatrix[9][1] = -0.250;
    fdnFeedbackMatrix[9][2] = 0.250;
    fdnFeedbackMatrix[9][3] = -0.250;
    fdnFeedbackMatrix[9][4] = 0.250;
    fdnFeedbackMatrix[9][5] = -0.250;
    fdnFeedbackMatrix[9][6] = 0.250;
    fdnFeedbackMatrix[9][7] = -0.250;
    fdnFeedbackMatrix[9][8] = -0.250;
    fdnFeedbackMatrix[9][9] = 0.250;
    fdnFeedbackMatrix[9][10] = -0.250;
    fdnFeedbackMatrix[9][11] = 0.250;
    fdnFeedbackMatrix[9][12] = -0.250;
    fdnFeedbackMatrix[9][13] = 0.250;
    fdnFeedbackMatrix[9][14] = -0.250;
    fdnFeedbackMatrix[9][15] = 0.250;
    fdnFeedbackMatrix[10][0] = 0.250;
    fdnFeedbackMatrix[10][1] = 0.250;
    fdnFeedbackMatrix[10][2] = -0.250;
    fdnFeedbackMatrix[10][3] = -0.250;
    fdnFeedbackMatrix[10][4] = 0.250;
    fdnFeedbackMatrix[10][5] = 0.250;
    fdnFeedbackMatrix[10][6] = -0.250;
    fdnFeedbackMatrix[10][7] = -0.250;
    fdnFeedbackMatrix[10][8] = -0.250;
    fdnFeedbackMatrix[10][9] = -0.250;
    fdnFeedbackMatrix[10][10] = 0.250;
    fdnFeedbackMatrix[10][11] = 0.250;
    fdnFeedbackMatrix[10][12] = -0.250;
    fdnFeedbackMatrix[10][13] = -0.250;
    fdnFeedbackMatrix[10][14] = 0.250;
    fdnFeedbackMatrix[10][15] = 0.250;
    fdnFeedbackMatrix[11][0] = 0.250;
    fdnFeedbackMatrix[11][1] = -0.250;
    fdnFeedbackMatrix[11][2] = -0.250;
    fdnFeedbackMatrix[11][3] = 0.250;
    fdnFeedbackMatrix[11][4] = 0.250;
    fdnFeedbackMatrix[11][5] = -0.250;
    fdnFeedbackMatrix[11][6] = -0.250;
    fdnFeedbackMatrix[11][7] = 0.250;
    fdnFeedbackMatrix[11][8] = -0.250;
    fdnFeedbackMatrix[11][9] = 0.250;
    fdnFeedbackMatrix[11][10] = 0.250;
    fdnFeedbackMatrix[11][11] = -0.250;
    fdnFeedbackMatrix[11][12] = -0.250;
    fdnFeedbackMatrix[11][13] = 0.250;
    fdnFeedbackMatrix[11][14] = 0.250;
    fdnFeedbackMatrix[11][15] = -0.250;
    fdnFeedbackMatrix[12][0] = 0.250;
    fdnFeedbackMatrix[12][1] = 0.250;
    fdnFeedbackMatrix[12][2] = 0.250;
    fdnFeedbackMatrix[12][3] = 0.250;
    fdnFeedbackMatrix[12][4] = -0.250;
    fdnFeedbackMatrix[12][5] = -0.250;
    fdnFeedbackMatrix[12][6] = -0.250;
    fdnFeedbackMatrix[12][7] = -0.250;
    fdnFeedbackMatrix[12][8] = -0.250;
    fdnFeedbackMatrix[12][9] = -0.250;
    fdnFeedbackMatrix[12][10] = -0.250;
    fdnFeedbackMatrix[12][11] = -0.250;
    fdnFeedbackMatrix[12][12] = 0.250;
    fdnFeedbackMatrix[12][13] = 0.250;
    fdnFeedbackMatrix[12][14] = 0.250;
    fdnFeedbackMatrix[12][15] = 0.250;
    fdnFeedbackMatrix[13][0] = 0.250;
    fdnFeedbackMatrix[13][1] = -0.250;
    fdnFeedbackMatrix[13][2] = 0.250;
    fdnFeedbackMatrix[13][3] = -0.250;
    fdnFeedbackMatrix[13][4] = -0.250;
    fdnFeedbackMatrix[13][5] = 0.250;
    fdnFeedbackMatrix[13][6] = -0.250;
    fdnFeedbackMatrix[13][7] = 0.250;
    fdnFeedbackMatrix[13][8] = -0.250;
    fdnFeedbackMatrix[13][9] = 0.250;
    fdnFeedbackMatrix[13][10] = -0.250;
    fdnFeedbackMatrix[13][11] = 0.250;
    fdnFeedbackMatrix[13][12] = 0.250;
    fdnFeedbackMatrix[13][13] = -0.250;
    fdnFeedbackMatrix[13][14] = 0.250;
    fdnFeedbackMatrix[13][15] = -0.250;
    fdnFeedbackMatrix[14][0] = 0.250;
    fdnFeedbackMatrix[14][1] = 0.250;
    fdnFeedbackMatrix[14][2] = -0.250;
    fdnFeedbackMatrix[14][3] = -0.250;
    fdnFeedbackMatrix[14][4] = -0.250;
    fdnFeedbackMatrix[14][5] = -0.250;
    fdnFeedbackMatrix[14][6] = 0.250;
    fdnFeedbackMatrix[14][7] = 0.250;
    fdnFeedbackMatrix[14][8] = -0.250;
    fdnFeedbackMatrix[14][9] = -0.250;
    fdnFeedbackMatrix[14][10] = 0.250;
    fdnFeedbackMatrix[14][11] = 0.250;
    fdnFeedbackMatrix[14][12] = 0.250;
    fdnFeedbackMatrix[14][13] = 0.250;
    fdnFeedbackMatrix[14][14] = -0.250;
    fdnFeedbackMatrix[14][15] = -0.250;
    fdnFeedbackMatrix[15][0] = 0.250;
    fdnFeedbackMatrix[15][1] = -0.250;
    fdnFeedbackMatrix[15][2] = -0.250;
    fdnFeedbackMatrix[15][3] = 0.250;
    fdnFeedbackMatrix[15][4] = -0.250;
    fdnFeedbackMatrix[15][5] = 0.250;
    fdnFeedbackMatrix[15][6] = 0.250;
    fdnFeedbackMatrix[15][7] = -0.250;
    fdnFeedbackMatrix[15][8] = -0.250;
    fdnFeedbackMatrix[15][9] = 0.250;
    fdnFeedbackMatrix[15][10] = 0.250;
    fdnFeedbackMatrix[15][11] = -0.250;
    fdnFeedbackMatrix[15][12] = 0.250;
    fdnFeedbackMatrix[15][13] = -0.250;
    fdnFeedbackMatrix[15][14] = -0.250;
    fdnFeedbackMatrix[15][15] = 0.250;
}
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbTail)
    
};

#endif // REVERBTAIL_H_INCLUDED
