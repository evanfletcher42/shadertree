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

#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GL/glext.h>

#include "treeinterface.h"

volatile sig_atomic_t stop = 0;
int serialPort;

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

int setupSerial( const char* serialPath )
{
    serialPort = open( serialPath, O_RDWR | O_NOCTTY | O_SYNC );
    if( serialPort < 0 )
    {
        fprintf(stderr, "Error opening serial port: %d (%s)\n", errno, strerror(errno));
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof tty);
    if (tcgetattr(serialPort, &tty) != 0) {
        fprintf(stderr, "Error in tcgetattr\n");
        close(serialPort);
        return 1;
    }

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
    tty.c_iflag &= ~IGNBRK;                         // disable break processing
    tty.c_lflag = 0;                                // no signaling chars, no echo
    tty.c_oflag = 0;                                // no remapping, no delays
    tty.c_cc[VMIN]  = 1;                            // read blocks until we get 1 byte
    tty.c_cc[VTIME] = 1;                            // 0.1 seconds read timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);         // shut off xon/xoff ctrl

    tty.c_cflag |= (CLOCAL | CREAD);                // ignore modem controls
    tty.c_cflag &= ~(PARENB | PARODD);              // shut off parity
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(serialPort, TCSANOW, &tty) != 0) {
        fprintf(stderr, "Error in tcsetattr\n");
        close(serialPort);
        return -1;
    }

    tcflush(serialPort, TCIOFLUSH);

    return serialPort;
}

void writeSerial( float* pfColors, uint8_t *pOutBuf, int numLEDs )
{
    pOutBuf[0] = '*';
    size_t writeSize = (size_t)(1 + 3 * numLEDs);

    for(int i = 0; i < numLEDs * 3; i++)
    {
        // gamma correct
        pfColors[i] = powf(pfColors[i], 1.7f);

        // to 8 bits, round, clamp
        pfColors[i] *= 255.0f + 0.5f;
        pfColors[i] = fmax(0.0f, fmin(255.0f, pfColors[i]));
        pOutBuf[i + 1] = (uint8_t)pfColors[i];
    }

    size_t n = write(serialPort, pOutBuf, writeSize);
    if( n != writeSize )
    {
        fprintf(stderr, "Serial write error (n=%zu)\n", n);
    }

    tcdrain(serialPort);
}

bool waitForReply()
{
    static char buf[1];
    int n = read(serialPort, buf, 1);

    if(n == 0)
    {
        // timeout
        return false;
    }

    if( n == 1 and buf[0] == 'X')
    {
        // ready
        return true;
    }

    // error
    fprintf(stderr, "Error reading from serial port: %d (%s) - returned %d\n", errno, strerror(errno), n);
    return false;
}

int main(int argc, char* argv[])
{   
    // Register signal handler
    signal(SIGTERM, handle_sigterm_sigint);
    signal(SIGINT, handle_sigterm_sigint);

    if( argc != 2 )
    {
        printf("Usage: %s <serial_port>\n", argv[0]);
        return 1;
    }

    const char* serialPath = argv[1];
    if( setupSerial( serialPath ) < 0)
    {
        return 1;
    }

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

    printf("Shader compiled successfully\n");

    // Get LED positions
    int numLEDs;
    float *ledPositions = getLEDPositions(numLEDs);
    if( !ledPositions )
    {
        printf("Failed to load LED positions\n");
        return -1;
    }

    printf("Loaded LEDs: %d\n", numLEDs);

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

    // Start main loop
    float startTime = static_cast<float>(getTimeSeconds());

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

        // float dt = static_cast<float>(getTimeSeconds()) - startTime - currentTime;
        // printf("dt: %f\n", dt);

        if( !bFirstWrite )
        {
            // Wait for ready signal. (Waiting here parallelizes shader eval with firmware busy)
            waitForReply();
        }
        bFirstWrite = false;

        // Send data to the tree
        writeSerial(pfColors, pOutBuf, numLEDs);

        // Handle any OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            fprintf(stderr, "OpenGL error: %d\n", error);
        }
    }

    // turn off the tree
    memset(pfColors, 0, sizeof(float) * 3 * numLEDs);
    writeSerial(pfColors, pOutBuf, numLEDs);

    close(serialPort);

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
