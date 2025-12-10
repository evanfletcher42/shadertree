#version 300 es
layout(location = 0) in vec3 iPos;
layout(location = 1) in uint iIdx;
uniform float iTime;
uniform int iFrame;
out vec3 outColor;

// (8, 4, 4)_2 augmented hadamard code
// maps 4 input bits -> 8 output bits
const int[16*8] hadamard = int[] (
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1,  1, -1,  1, -1,  1, -1,  1,
    -1, -1,  1,  1, -1, -1,  1,  1,
    -1,  1,  1, -1, -1,  1,  1, -1,
    -1, -1, -1, -1,  1,  1,  1,  1,
    -1,  1, -1,  1,  1, -1,  1, -1,
    -1, -1,  1,  1,  1,  1, -1, -1,
    -1,  1,  1, -1,  1, -1, -1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,
     1, -1,  1, -1,  1, -1,  1, -1,
     1,  1, -1, -1,  1,  1, -1, -1,
     1, -1, -1,  1,  1, -1, -1,  1,
     1,  1,  1,  1, -1, -1, -1, -1,
     1, -1,  1, -1, -1,  1, -1,  1,
     1,  1, -1, -1, -1, -1,  1,  1,
     1, -1, -1,  1, -1,  1,  1, -1
);

// Precomputed cumulative sum of above table across 8 codeword bits (for LED cycle offset)
const int[16*8] hSum = int[] (
    -1, -2, -3, -4, -5, -6, -7, -8,
    -1,  0, -1,  0, -1,  0, -1,  0,
    -1, -2, -1,  0, -1, -2, -1,  0,
    -1,  0,  1,  0, -1,  0,  1,  0,
    -1, -2, -3, -4, -3, -2, -1,  0,
    -1,  0, -1,  0,  1,  0,  1,  0,
    -1, -2, -1,  0,  1,  2,  1,  0,
    -1,  0,  1,  0,  1,  0, -1,  0,
     1,  2,  3,  4,  5,  6,  7,  8,
     1,  0,  1,  0,  1,  0,  1,  0,
     1,  2,  1,  0,  1,  2,  1,  0,
     1,  0, -1,  0,  1,  0, -1,  0,
     1,  2,  3,  4,  3,  2,  1,  0,
     1,  0,  1,  0, -1,  0, -1,  0,
     1,  2,  1,  0, -1, -2, -1,  0,
     1,  0, -1,  0, -1,  0,  1,  0
);

void main() {
    // Blink out encoded colors.
    //
    // We have a 16-bit LED index we are trying to communicate as a message.
    // Each nibble (4) bits is encoded as an 8-bit hadamard code.
    // We blink out four 8-bit codes in sequence (4 blocks * 8 bits/block = 32 codeword bits, for 16 message bits)
    // 
    // Bits "on the wire" are encoded as color transitions, where:
    //  - a positive (R->G->B->R) transition is a 1
    //  - a negative (R->B->G->R) transition is a 0
    // 
    // We blink out 33 colors (32 transitions), plus two white frames for sync (total 35 colors).
    //
    // The hSum table above gives us the cumulative sum of the hadamard table, to use as the offset for each nibble.

    // Index in full 35-slot sequence

    int iFloatFrame = int(iTime * 59.94f);
    int iOuterIdx = int(iFloatFrame / 2 ) % 35;

    //int iOuterIdx = (iFrame / 4) % 35;
    //int iOuterIdx = (iFrame / 64) % 35;  // slower for human debug

    // illuminate in subsets
    //int iSubsetIdx = (int(iIdx) + (iFloatFrame / 2 / 35 ) ) % 10;

    //if( iSubsetIdx > 2)
   // {
       // outColor = vec3(0.0f, 0.0f, 0.0f);
        //return;
    //}



    if( iOuterIdx < 2 )
    {
        // First two are sync pulse
        outColor = vec3( 1.0f, 1.0f, 1.0f );
        return;
    } else
    {
        outColor = vec3(0.0f, 0.0f, 0.0f);
    }

    //if( iSubsetIdx == 2)
    //{
        //outColor = vec3(0.0f, 0.0f, 0.0f);
        //return;
    //}

    int iColorIdx = iOuterIdx - 2;  // Color index, [0...32]

    // RGB cycle offset this LED, [0...2]
    // Init to index-mod-3 to keep colors more random / as an extra checksum of sorts
    int cOfs = int(iIdx) % 3;

    if( iColorIdx > 0 )
    {
        // transitions
        int iTransitionIdx = iColorIdx - 1;

        int iNibbleIdx = iTransitionIdx / 8;  // which of the four nibbles we're on
        int iNibbleOfs = iTransitionIdx % 8;  // which bit in the codeword we're on

        // break up led index (16 bits) into nibbles
        uvec4 nibbles = uvec4(
            ( iIdx >> 12 ) & 0xFu,
            ( iIdx >> 8 ) & 0xFu,
            ( iIdx >> 4 ) & 0xFu,
            ( iIdx      ) & 0xFu );

        // Add up offsets from previous nibbles
        if( iNibbleIdx > 0 )
            cOfs += hSum[nibbles[0]*8u + 7u];
        if( iNibbleIdx > 1 )
            cOfs += hSum[nibbles[1]*8u + 7u];
        if( iNibbleIdx > 2 )
            cOfs += hSum[nibbles[2]*8u + 7u];

        cOfs += hSum[nibbles[iNibbleIdx]*8u + uint(iNibbleOfs)];
    }

    // To color cycle offset.
    // +33 -> sane modulo only. 4x zeros have value -32 at end of sequence
    cOfs = (cOfs + 33) % 3;

    if( cOfs == 0)
        outColor = vec3(1.0f, 0.0f, 0.0f);
    if( cOfs == 1)
        outColor = vec3(0.0f, 1.0f, 0.0f);
    if( cOfs == 2)
        outColor = vec3(0.0f, 0.0f, 1.0f);
}
