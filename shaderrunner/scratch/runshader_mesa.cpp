// led_shader_runner.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_GLEXT_PROTOTYPES 
#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/osmesa.h>

// Number of LEDs (adjust as needed)
#define NUM_LEDS 1000

// Structure for 3D vectors
typedef struct {
    float x, y, z;
} Vec3;

// Placeholder function to send RGB data to the LED tree
void sendColorsToTree(Vec3* colors, int numLEDs) {
    // Implement your USB communication here
    // For demonstration, we'll print the first few colors
    for (int i = 0; i < (numLEDs < 5 ? numLEDs : 5); i++) {
        printf("LED %d: R=%f, G=%f, B=%f\n", i, colors[i].x, colors[i].y, colors[i].z);
    }
}

// Function to load LED positions from a file
Vec3* loadLEDPositions(const char* filename, int numLEDs) {
    Vec3* positions = (Vec3*)malloc(sizeof(Vec3) * numLEDs);
    if (!positions) {
        fprintf(stderr, "Failed to allocate memory for LED positions.\n");
        exit(EXIT_FAILURE);
    }

    FILE* file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Failed to open LED positions file: %s\n", filename);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < numLEDs; i++) {
        if (fscanf(file, "%f %f %f", &positions[i].x, &positions[i].y, &positions[i].z) != 3) {
            fprintf(stderr, "Error reading LED position at index %d\n", i);
            fclose(file);
            exit(EXIT_FAILURE);
        }
    }

    fclose(file);
    return positions;
}

// Function to load shader code from a file
char* loadShaderCode(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Failed to open shader file: %s\n", filename);
        exit(EXIT_FAILURE);
    }

    fseek(file, 0L, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char* code = (char*)malloc(size + 1);
    if (!code) {
        fprintf(stderr, "Failed to allocate memory for shader code.\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }

    fread(code, 1, size, file);
    code[size] = '\0';

    fclose(file);
    return code;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <led_positions.txt> <shader.glsl>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* positionsFile = argv[1];
    const char* shaderFile = argv[2];

    // Load LED positions
    Vec3* ledPositions = loadLEDPositions(positionsFile, NUM_LEDS);

    // Load shader code
    char* userShaderCode = loadShaderCode(shaderFile);

    // Initialize OSMesa context
    OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 0, 0, 0, NULL);
    if (!ctx) {
        fprintf(stderr, "OSMesaCreateContext failed.\n");
        return EXIT_FAILURE;
    }

    // Allocate buffer for OSMesa context
    int width = 1, height = 1;
    void* buffer = malloc(width * height * 4); // RGBA buffer
    if (!buffer) {
        fprintf(stderr, "Failed to allocate buffer.\n");
        return EXIT_FAILURE;
    }

    if (!OSMesaMakeCurrent(ctx, buffer, GL_UNSIGNED_BYTE, width, height)) {
        fprintf(stderr, "OSMesaMakeCurrent failed.\n");
        return EXIT_FAILURE;
    }

    // Initialize GLEW
    GLenum glewInitResult = glewInit();
    if (GLEW_OK != glewInitResult) {
        fprintf(stderr, "GLEW initialization failed: %s\n", glewGetErrorString(glewInitResult));
        return EXIT_FAILURE;
    }

    // Create and bind VBO for LED positions
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, NUM_LEDS * sizeof(Vec3), ledPositions, GL_STATIC_DRAW);

    // Create and compile the vertex shader
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    const GLchar* vertexShaderSource[] = {
        "#version 330 core\n",
        "layout(location = 0) in vec3 position;\n",
        "uniform float time;\n",
        "out vec3 color;\n",
        "\n",
        // User-provided shader code
        userShaderCode,
        "\n"
    };
    glShaderSource(vertexShader, 6, vertexShaderSource, NULL);
    glCompileShader(vertexShader);

    // Check for vertex shader compilation errors
    GLint success;
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        fprintf(stderr, "Vertex Shader compilation failed:\n%s\n", infoLog);
        return EXIT_FAILURE;
    }

    // Create and compile the fragment shader
    const GLchar* fragmentShaderSource = "#version 330 core\nvoid main() {}\n";
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);

    // Check for fragment shader compilation errors
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        fprintf(stderr, "Fragment Shader compilation failed:\n%s\n", infoLog);
        return EXIT_FAILURE;
    }

    // Create shader program and attach shaders
    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);

    // Specify the output variables for transform feedback
    const GLchar* feedbackVaryings[] = { "color" };
    glTransformFeedbackVaryings(shaderProgram, 1, feedbackVaryings, GL_INTERLEAVED_ATTRIBS);

    // Link the shader program
    glLinkProgram(shaderProgram);

    // Check for linking errors
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        fprintf(stderr, "Shader Program linking failed:\n%s\n", infoLog);
        return EXIT_FAILURE;
    }

    // Use the shader program
    glUseProgram(shaderProgram);

    // Set the time uniform
    GLint timeUniformLocation = glGetUniformLocation(shaderProgram, "time");
    float currentTime = 0.0f; // Replace with actual time as needed
    glUniform1f(timeUniformLocation, currentTime);

    // Bind the VBO and set up vertex attribute
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    GLint posAttrib = glGetAttribLocation(shaderProgram, "position");
    glEnableVertexAttribArray(posAttrib);
    glVertexAttribPointer(posAttrib, 3, GL_FLOAT, GL_FALSE, 0, 0);

    // Create and bind Transform Feedback Buffer Object (TBO)
    GLuint tbo;
    glGenBuffers(1, &tbo);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tbo);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, NUM_LEDS * sizeof(Vec3), NULL, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tbo);

    // Run the shader program with transform feedback
    glEnable(GL_RASTERIZER_DISCARD); // Disable rasterization
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, NUM_LEDS);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);

    // Map the transform feedback buffer to access the output data
    Vec3* colors = (Vec3*)glMapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, GL_READ_ONLY);
    if (!colors) {
        fprintf(stderr, "Failed to map Transform Feedback Buffer.\n");
        return EXIT_FAILURE;
    }

    // Send the colors to the LED tree
    sendColorsToTree(colors, NUM_LEDS);

    // Unmap the buffer
    glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);

    // Clean up
    glDeleteProgram(shaderProgram);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &tbo);

    OSMesaDestroyContext(ctx);
    free(buffer);
    free(ledPositions);
    free(userShaderCode);

    return EXIT_SUCCESS;
}
