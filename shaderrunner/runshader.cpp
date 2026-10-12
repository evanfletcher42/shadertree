#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <termios.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <thread>
#include <mutex>
#include <condition_variable>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GL/glext.h>

#define NR_LEDS_PER_STRAND	 (215)
#define block_size			 512
#define STRANDS				32
#define USB_TIMEOUT 1024
#define TRANSFERS 8
#define DEVICES 2
const char * serials[DEVICES] = { "f38dabcd5b6ebc14", "d3f9abcd3bdabc14" };

#include "x32breakoutdriver.h"

volatile sig_atomic_t stop = 0;
volatile sig_atomic_t stopLED = 0;

// --no-usb: render as normal but don't touch USB
bool g_bNoUsb = false;

og_sema_t semaReadyForLEDs;
og_sema_t semaDoneLEDs;

double getTimeSeconds()
{
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    auto now = clock::now();
    std::chrono::duration<double> d = now - t0;
    return d.count();
}

void handle_sigterm_sigint(int signum)
{
    stop = 1;
}

GLuint compileShader(const std::string &shaderCode, GLenum shaderType)
{
    GLuint shader = glCreateShader(shaderType);
    const char *code = shaderCode.c_str();
    glShaderSource(shader, 1, &code, NULL);
    glCompileShader(shader);

    // Check for compilation errors
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        // Retrieve error message
        GLint logSize = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logSize);
        char *infoLog = new char[logSize];
        glGetShaderInfoLog(shader, logSize, NULL, infoLog);
        fprintf(stderr, "Shader compilation failed:\n%s\n", infoLog);
        delete[] infoLog;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader)
{
    GLuint program = glCreateProgram();

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);

    // Specify transform feedback varyings before linking
    const GLchar *varyings[] = {"outColor"}; // Output variable in the vertex shader
    glTransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);

    glLinkProgram(program);

    // Check for linking errors
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        // Retrieve error message
        GLint logSize = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logSize);
        char *infoLog = new char[logSize];
        glGetProgramInfoLog(program, logSize, NULL, infoLog);
        fprintf(stderr, "Program linking failed:\n%s\n", infoLog);
        delete[] infoLog;
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

float *getLEDPositions(int &numLEDs)
{
    FILE* f = fopen("ledcoords.dat", "rb");
    if( !f ) {
        printf("Error loading file: %d (%s)\n", errno, strerror(errno));
        return nullptr;
    }

    // measure file size
    if( fseek(f, 0, SEEK_END) != 0 )
    {
        printf("Failed to seek to EOF: %d (%s)\n", errno, strerror(errno));
        fclose(f);
        return nullptr;
    }

    long fsize = ftell( f );
    if( fsize == -1L )
    {
        printf("Error measuring file size: %d (%s)\n", errno, strerror(errno));
        fclose(f);
        return nullptr;
    }
    printf("fsize: %ld\n", fsize);

    numLEDs = (int)(fsize / sizeof(float) / 3);
    rewind(f);

    float *positions = new float[numLEDs * 3];
    size_t nRead = fread(positions, sizeof(float), numLEDs * 3, f);
    if( nRead != ( size_t )( numLEDs * 3 ) )
    {
        printf("Error reading data: Expected %d floats, got %zu\n", numLEDs * 3, nRead );
        delete[] positions;
        fclose(f);
        return nullptr;
    }

    fclose(f);
    return positions;
}

void USBThread()
{
    printf("SetupBreakoutDriver\n");
    SetupBreakoutDriver();

    while( !stopLED )
    {
        // printf("TickBreakoutDriver\n");
        TickBreakoutDriver();
        if( done_frame == done_mask)
        {
            OGUnlockSema(semaReadyForLEDs);
            OGLockSema(semaDoneLEDs);

            done_frame = 0;
        }
    }

    printf("CleanupBreakoutDriver\n");
    CleanupBreakoutDriver();
    printf("CleanupBreakoutDriver Done\n");
}

void WriteUsb(float *pfColors, int numLEDs)
{
    if ( g_bNoUsb )
    {
        // stand in for the WS2812 frame time: 30 us per LED, plus the reset gap
        std::this_thread::sleep_for( std::chrono::microseconds( NR_LEDS_PER_STRAND * 30 + 300 ) );
        return;
    }

    // Wait for USB ready
    OGLockSema(semaReadyForLEDs);

    // Copy over LEDs
    for (int i = 0; i < numLEDs; i++)
    {
        float flR = pfColors[i * 3 + 0];
        float flG = pfColors[i * 3 + 1];
        float flB = pfColors[i * 3 + 2];

        // clamp
        flR = flR > 1.0f ? 1.0f : (flR < 0 ? 0 : flR);
        flG = flG > 1.0f ? 1.0f : (flG < 0 ? 0 : flG);
        flB = flB > 1.0f ? 1.0f : (flB < 0 ? 0 : flB);

        // gamma correct
        flR = powf(flR, 1.7f);
        flG = powf(flG, 1.7f);
        flB = powf(flB, 1.7f);

        // scale
        int r = (int)(flR * 255.f + 0.5f);
        int g = (int)(flG * 255.f + 0.5f);
        int b = (int)(flB * 255.f + 0.5f);

        // address demuxing
        int nDevice = i / (NR_LEDS_PER_STRAND * STRANDS);
        int nStrand = (i / NR_LEDS_PER_STRAND) % STRANDS;
        int nIdx = i % NR_LEDS_PER_STRAND;

        //printf("nDevice %d nStrand %d nIdx %d\n", nDevice, nStrand, nIdx);

        LEDs[nDevice][nIdx][nStrand] = b | (g << 8) | (r << 16);
    }

    OGUnlockSema(semaDoneLEDs);
}

int main(int argc, char* argv[])
{   
    // Register signal handler
    signal(SIGTERM, handle_sigterm_sigint);
    signal(SIGINT, handle_sigterm_sigint);

    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--no-usb" ) == 0 )
            g_bNoUsb = true;
    }

    semaReadyForLEDs = OGCreateSema();
    semaDoneLEDs = OGCreateSema();

    // Read vertex shader code from stdin
    std::string vertexShaderCode;
    {
        std::ostringstream oss;
        oss << std::cin.rdbuf();
        vertexShaderCode = oss.str();
    }

    // ----- Create headless OpenGL context via OSMesa -----
    int ctx_attribs[] = {
        OSMESA_PROFILE,          
        OSMESA_CORE_PROFILE,
        OSMESA_CONTEXT_MAJOR_VERSION, 3,
        OSMESA_CONTEXT_MINOR_VERSION, 3,
        0
    };

    OSMesaContext ctx = OSMesaCreateContextAttribs(ctx_attribs, NULL);
    if (!ctx)
    {
        fprintf(stderr, "OSMesaCreateContextAttribs failed\n");
        return -1;
    }

    // We never actually rasterize (we always enable GL_RASTERIZER_DISCARD),
    // but OSMesa requires a buffer to attach. 1×1 RGBA is enough.
    int width = 1, height = 1;
    void *osmesaBuffer = malloc(width * height * 4); // 4 bytes per pixel (RGBA8)
    if (!osmesaBuffer)
    {
        fprintf(stderr, "Failed to allocate OSMesa buffer\n");
        OSMesaDestroyContext(ctx);
        return -1;
    }

    if (!OSMesaMakeCurrent(ctx, osmesaBuffer, GL_UNSIGNED_BYTE, width, height))
    {
        fprintf(stderr, "OSMesaMakeCurrent failed\n");
        free(osmesaBuffer);
        OSMesaDestroyContext(ctx);
        return -1;
    }
    // ----- End OSMesa init -----

    // Compile the user-supplied vertex shader
    GLuint vertexShader = compileShader(vertexShaderCode, GL_VERTEX_SHADER);
    if (vertexShader == 0)
    {
        OSMesaDestroyContext(ctx);
        return 1;
    }

    // Minimal fragment shader
    const char *fragmentShaderSource = "#version 300 es\n void main() { }";
    GLuint fragmentShader = compileShader(fragmentShaderSource, GL_FRAGMENT_SHADER);
    if (fragmentShader == 0)
    {
        OSMesaDestroyContext(ctx);
        return 2;
    }

    // Link shaders into a program
    GLuint shaderProgram = linkProgram(vertexShader, fragmentShader);
    if (shaderProgram == 0)
    {
        OSMesaDestroyContext(ctx);
        return 3;
    }

    // runner_process.py waits for this line before marking the shader as playing
    printf("Shader compiled successfully\n");
    fflush(stdout);

    // Get LED positions
    int numLEDs;
    float *ledPositions = getLEDPositions(numLEDs);
    if( !ledPositions )
    {
        printf("Failed to load LED positions\n");
        return -1;
    }

    printf("Loaded LEDs: %d\n", numLEDs);

    // started after all early exits; returning with a joinable thread aborts
    std::thread thread;
    if ( !g_bNoUsb )
        thread = std::thread(USBThread);

    // Create the index buffer
    GLuint* pIndices = new GLuint[numLEDs];
    for (GLuint i = 0; i < (GLuint)numLEDs; i++)
    {
        pIndices[i] = i;
    }

    // Output buffer: starts with command character '*', then RGB values for all LEDs
    uint8_t *pOutBuf = new uint8_t[numLEDs * 3 + 1];

    // Create VBO for LED positions
    GLuint VBO;
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, numLEDs * 3 * sizeof(float), ledPositions, GL_STATIC_DRAW);

    // Create VBO for indices
    GLuint indexVBO;
    glGenBuffers(1, &indexVBO);
    glBindBuffer(GL_ARRAY_BUFFER, indexVBO);
    glBufferData(GL_ARRAY_BUFFER, numLEDs * sizeof(GLuint), pIndices, GL_STATIC_DRAW);

    // Create VAO
    GLuint VAO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    // Position (location = 0)
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void *)0);

    // Index (location = 1)
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, indexVBO);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, 0, (void*)0);

    // Set up transform feedback buffer
    GLuint feedbackBuffer;
    glGenBuffers(1, &feedbackBuffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedbackBuffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, numLEDs * 3 * sizeof(float), NULL, GL_STATIC_READ);

    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedbackBuffer);

    // Get uniform location for time
    GLint timeUniformLocation = glGetUniformLocation(shaderProgram, "iTime");
    if (timeUniformLocation == -1)
    {
        fprintf(stderr, "Warning: 'iTime' uniform not found in shader.\n");
    }

    GLint frameUniformLocation = glGetUniformLocation(shaderProgram, "iFrame");
    if (frameUniformLocation == -1)
    {
        fprintf(stderr, "Warning: 'iFrame' uniform not found in shader.\n");
    }

    // Start main loop
    float startTime = static_cast<float>(getTimeSeconds());
    int nFrameCounter = 0;

    float *pfColors = new float[numLEDs * 3];

    bool bFirstWrite = true;

    while (!stop)
    {
        float currentTime = static_cast<float>(getTimeSeconds()) - startTime;

        // Use the shader program
        glUseProgram(shaderProgram);

        // Update time uniform
        if (timeUniformLocation != -1)
        {
            glUniform1f(timeUniformLocation, currentTime);
        }

        // Update frame uniform
        if (frameUniformLocation != -1)
        {
            glUniform1i(frameUniformLocation, nFrameCounter);
        }

        // Disable rasterization since we're not rendering
        glEnable(GL_RASTERIZER_DISCARD);

        glBindVertexArray(VAO);

        // Begin transform feedback
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, numLEDs);
        glEndTransformFeedback();

        glDisable(GL_RASTERIZER_DISCARD);

        // Read back the data
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedbackBuffer);
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, numLEDs * 3 * sizeof(float), pfColors);

        // Handle any OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            fprintf(stderr, "OpenGL error: %d\n", error);
        }

        // GLSL is done - A new frame is ready (in pfColors)

        WriteUsb(pfColors, numLEDs);

        nFrameCounter++;
    }

    printf("Quit - Shutdown tree\n");

    // turn off the tree
    memset(pfColors, 0, sizeof(float) * 3 * numLEDs);

    WriteUsb(pfColors, numLEDs);
    WriteUsb(pfColors, numLEDs);
    printf("Last write done\n");

    usleep(30000);

    printf("Signal USB thread stop\n");
    stopLED = 1;
    OGUnlockSema(semaDoneLEDs);


    if ( thread.joinable() )
        thread.join();
    printf("Joined USB thread\n");

    delete[] pfColors;
    delete[] pIndices;
    delete[] pOutBuf;

    // Clean up resources
    delete[] ledPositions;
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &indexVBO);
    glDeleteBuffers(1, &feedbackBuffer);
    glDeleteVertexArrays(1, &VAO);
    glDeleteProgram(shaderProgram);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    OSMesaDestroyContext(ctx);
    free(osmesaBuffer);

    printf("Goodbye\n");

    return 0;
}
