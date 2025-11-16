#include <stdio.h>

void WriteToTree( float* pflColors, int n )
{
    printf("Write colors:\n");
    for(int i = 0; i < (n > 5 ? 5 : n); i++)
    {
        printf("\t%f %f %f\n", pflColors[i*3 + 0], pflColors[i*3 + 1], pflColors[i*3 + 2]);
    }
    printf("\n\n");
}
