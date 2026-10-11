// app.js

// Grab editor template data
data = document.currentScript.dataset;
const csrfToken = data.csrftoken;
if(!csrfToken)
    console.error("CSRF token not found");


const shaderIdent = data.shaderident;

// ------------------------
// Initialize Ace Editor
// ------------------------
const editor = ace.edit("editor");
editor.setTheme("ace/theme/chrome");
editor.session.setMode("ace/mode/glsl");

const shaderPrefix = data.shaderprefix;
const prefixLines = shaderPrefix.split('\n').length;

editor.setValue(data.editortext, -1);  // Set the fetched text as the editor's content

// template embeds LED positions directly into global scope
const positions = window.positions

// Number of LEDs
const numLEDs = positions.length / 3;

// ------------------------
// Initialize WebGL2 Contexts
// ------------------------

const canvas = document.getElementById('glCanvas');
const gl = canvas.getContext('webgl2');

if (!gl) {
    alert("WebGL2 is not supported in your browser.");
}

// Error logging
const errorLog = document.getElementById('errorLog');

function showError(message) {
    errorLog.textContent = message;
}

function clearError() {
    errorLog.textContent = '';
}

function clearErrorMarkers() {
  editor.getSession().clearAnnotations();
  const markers = editor.getSession().getMarkers(false);
  Object.values(markers).forEach((marker) => {
    editor.getSession().removeMarker(marker.id);
  });
}

function highlightErrorLines(errorMessage) {
  clearErrorMarkers();

  const annotations = [];
  const lines = errorMessage.split('\n');

  lines.forEach((line) => {
      const match = line.match(/ERROR: (\d+):(\d+)/);
      if (match) {
          const lineNumber = parseInt(match[2], 10) - prefixLines; // Ace is 0-indexed and the editor lacks the shader prefix
          const errorText = (lineNumber + 1) + line.substring(match[0].length).trim();

          // Set annotation
          annotations.push({
              row: lineNumber,
              column: 0,
              text: errorText,
              type: "error" // or "warning" for different marker colors
          });

          // Add a red highlight for the entire line
          const Range = ace.require("ace/range").Range;
          editor.getSession().addMarker(
              new Range(lineNumber, 0, lineNumber, 1),
              "error-highlight",
              "fullLine"
          );
      }
  });

  // Apply annotations to show errors in the gutter
  editor.getSession().setAnnotations(annotations);
}

// ------------------------
// Shader Compilation Utilities
// ------------------------

function compileShader(source, type) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    const success = gl.getShaderParameter(shader, gl.COMPILE_STATUS);
    if (!success) {
        const info = gl.getShaderInfoLog(shader);
        gl.deleteShader(shader);
        throw new Error('Shader compilation failed:\n' + info);
    }
    return shader;
}

function createProgram(vertexSource, fragmentSource, transformFeedbackVaryings = null) {
    const program = gl.createProgram();
    const vertexShader = compileShader(vertexSource, gl.VERTEX_SHADER);
    const fragmentShader = compileShader(fragmentSource, gl.FRAGMENT_SHADER);

    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);

    if (transformFeedbackVaryings) {
        gl.transformFeedbackVaryings(program, transformFeedbackVaryings, gl.INTERLEAVED_ATTRIBS);
    }

    gl.linkProgram(program);

    const success = gl.getProgramParameter(program, gl.LINK_STATUS);
    if (!success) {
        const info = gl.getProgramInfoLog(program);
        gl.deleteProgram(program);
        throw new Error('Program linking failed:\n' + info);
    }
    return program;
}

// ------------------------
// Create Buffers
// ------------------------

// Position Buffer
const positionBuffer = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(positions), gl.STATIC_DRAW);

// Index buffer
const indices = new Uint32Array(numLEDs);
for(let i = 0; i < numLEDs; i++)
{
    indices[i] = i;
}
const indexBuffer = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, indexBuffer);
gl.bufferData(gl.ARRAY_BUFFER, indices, gl.STATIC_DRAW);

// ------------------------
// Transform Feedback Setup
// ------------------------

// Function to compile user's shader and perform transform feedback
function compileUserShader() {
    const userVertexSource = shaderPrefix + editor.getValue();
    const dummyFragmentSource = `#version 300 es
precision highp float;
void main() {}
`;

    // Specify the varyings to capture
    const varyings = ['outColor'];

    // Create transform feedback program
    const tfProgram = createProgram(userVertexSource, dummyFragmentSource, varyings);

    return tfProgram;
}

// ------------------------
// Render Program Setup
// ------------------------

// vertex shader for UI preview
// Output colors from the user's shader are used here
const renderVertexSource = `#version 300 es
precision highp float;

// Input vertex position
in vec3 aPos;

// Input vertex color
in vec3 aColor;

// Uniform matrices
uniform mat4 uProjectionMatrix;
uniform mat4 uViewMatrix;

// Output color to fragment shader
out vec3 vColor;

void main() {
    vec4 viewPos = uViewMatrix * vec4(aPos, 1.0);
    float zd = max(0.01f, -viewPos.z);
    float f = uProjectionMatrix[0][0];

    gl_Position = uProjectionMatrix * viewPos;
    gl_PointSize = 10.0f * f / zd;
    vColor = aColor;
}
`;

// fragment shader for UI preview
const renderFragmentSource = `#version 300 es
precision highp float;

in vec3 vColor;
out vec4 fragColor;

void main() {
    float dist = distance(gl_PointCoord, vec2(0.5, 0.5));
    float alpha = 1.0 - smoothstep(0.0, 0.45, dist);
    fragColor = vec4(vColor, alpha);
}
`;

// Create render program
const renderProgram = createProgram(renderVertexSource, renderFragmentSource);

// Get attribute and uniform locations for render program
const renderAttribLocations = {
    position: gl.getAttribLocation(renderProgram, 'aPos'),
    color: gl.getAttribLocation(renderProgram, 'aColor'),
};

const renderUniformLocations = {
    projectionMatrix: gl.getUniformLocation(renderProgram, 'uProjectionMatrix'),
    viewMatrix: gl.getUniformLocation(renderProgram, 'uViewMatrix'),
};

// ------------------------
// Transform Feedback Buffer Setup
// ------------------------

// Create buffer to store colors from transform feedback
const colorBuffer = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
gl.bufferData(gl.ARRAY_BUFFER, numLEDs * 3 * Float32Array.BYTES_PER_ELEMENT, gl.DYNAMIC_COPY);

// ------------------------
// Prepare Render VAO
// ------------------------

const renderVAO = gl.createVertexArray();
gl.bindVertexArray(renderVAO);

// Bind position buffer
gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
gl.enableVertexAttribArray(renderAttribLocations.position);
gl.vertexAttribPointer(renderAttribLocations.position, 3, gl.FLOAT, false, 0, 0);

// Bind color buffer
gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
gl.enableVertexAttribArray(renderAttribLocations.color);
gl.vertexAttribPointer(renderAttribLocations.color, 3, gl.FLOAT, false, 0, 0);

// Unbind VAO
gl.bindVertexArray(null);

// ------------------------
// Capture Colors with Transform Feedback
// ------------------------

function captureColors(tfProgram) {
    if (!tfProgram) return;

    // Create and bind a Vertex Array Object for transform feedback
    const tfCaptureVAO = gl.createVertexArray();
    gl.bindVertexArray(tfCaptureVAO);

    // Bind position buffer
    gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);

    // Bind index buffer
    gl.bindBuffer(gl.ARRAY_BUFFER, indexBuffer);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribIPointer(1, 1, gl.UNSIGNED_INT, 0, 0);

    // Bind color buffer for transform feedback
    gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER, 0, colorBuffer);

    // Use the transform feedback program
    gl.useProgram(tfProgram);

    // Set uniforms
    const iTimeLocation = gl.getUniformLocation(tfProgram, 'iTime');
    if (iTimeLocation != null) {
        gl.uniform1f(iTimeLocation, (performance.now() - startTime) / 1000.0);
    }

    const frameIdxLocation = gl.getUniformLocation(tfProgram, 'iFrame');
    if (frameIdxLocation != null) {
        gl.uniform1i(frameIdxLocation, frameIdx);
    }

    // Disable rasterization
    gl.enable(gl.RASTERIZER_DISCARD);

    // Begin transform feedback
    gl.beginTransformFeedback(gl.POINTS);

    // Draw points to capture colors
    gl.drawArrays(gl.POINTS, 0, numLEDs);

    // End transform feedback
    gl.endTransformFeedback();

    // Re-enable rasterization
    gl.disable(gl.RASTERIZER_DISCARD);

    // Unbind VAO and program
    gl.bindVertexArray(null);
    gl.useProgram(null);

    // Unbind the transform feedback buffer to prevent conflicts
    gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER, 0, null);

    // Clean up the temporary VAO
    gl.deleteVertexArray(tfCaptureVAO);

    frameIdx++;
}

// ------------------------
// Rendering Loop
// ------------------------

let tfProgram; // Transform Feedback Program
let startTime = performance.now();
let frameIdx = 0;

let bUserShaderCompiles = false;

// Whether the user has unsaved changes
let isDirty = false;

// Function to compile user shader and set up transform feedback
function setupTransformFeedbackProgram() {
    try {
        tfProgram = compileUserShader();
        clearError();
        clearErrorMarkers();
        bUserShaderCompiles = true;
    } catch (e) {
        showError(e.message);
        highlightErrorLines(e.message);
        tfProgram = null;
        bUserShaderCompiles = false;
    }
}

// Function to render the point cloud
function render() {
    // Resize canvas
    resizeCanvasToDisplaySize(gl.canvas);

    // Clear the canvas
    gl.viewport(0, 0, gl.canvas.width, gl.canvas.height);
    gl.clearColor(0.125, 0.125, 0.125, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    // Enable depth testing
    gl.enable(gl.DEPTH_TEST);

    // Capture colors using Transform Feedback
    if (tfProgram) {
        captureColors(tfProgram);
    }

    // Use render program
    gl.useProgram(renderProgram);

    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE);
    gl.disable(gl.DEPTH_TEST);


    // Set up projection matrix
    const aspect = gl.canvas.clientWidth / gl.canvas.clientHeight;
    const projectionMatrix = mat4.create();
    mat4.perspective(projectionMatrix, 45 * Math.PI / 180, aspect, 0.1, 100.0);
    gl.uniformMatrix4fv(renderUniformLocations.projectionMatrix, false, projectionMatrix);

    // Set up view matrix
    const viewMatrix = mat4.create();
    mat4.translate(viewMatrix, viewMatrix, [0, 0, -cameraDistance]);
    mat4.rotateX(viewMatrix, viewMatrix, rotationX);
    mat4.rotateY(viewMatrix, viewMatrix, rotationY);
    gl.uniformMatrix4fv(renderUniformLocations.viewMatrix, false, viewMatrix);

    // Bind render VAO
    gl.bindVertexArray(renderVAO);

    // Draw points
    gl.drawArrays(gl.POINTS, 0, numLEDs);

    // Unbind VAO and program
    gl.bindVertexArray(null);
    gl.useProgram(null);

    requestAnimationFrame(render);
}

// Function to resize the canvas
function resizeCanvasToDisplaySize(canvas) {
    const displayWidth  = canvas.clientWidth;
    const displayHeight = canvas.clientHeight;
    if (canvas.width !== displayWidth || canvas.height !== displayHeight) {
        canvas.width  = displayWidth;
        canvas.height = displayHeight;
    }
}

// ------------------------
// Camera Controls
// ------------------------

let cameraDistance = 5.0;
let rotationX = 0;
let rotationY = 0;
let isDragging = false;
let lastMouseX;
let lastMouseY;

// Mouse events for rotation
canvas.addEventListener('mousedown', (e) => {
    isDragging = true;
    lastMouseX = e.clientX;
    lastMouseY = e.clientY;
});

canvas.addEventListener('mousemove', (e) => {
    if (isDragging) {
        const deltaX = e.clientX - lastMouseX;
        const deltaY = e.clientY - lastMouseY;
        rotationY += deltaX * 0.005;
        rotationX += deltaY * 0.005;
        lastMouseX = e.clientX;
        lastMouseY = e.clientY;
    }
});

canvas.addEventListener('mouseup', () => {
    isDragging = false;
});

canvas.addEventListener('mouseleave', () => {
    isDragging = false;
});

// Wheel event for zoom
canvas.addEventListener('wheel', (e) => {
    event.preventDefault();
    cameraDistance += e.deltaY * 0.01;
    cameraDistance = Math.max(0.1, Math.min(10.0, cameraDistance));
});

// ------------------------
// Shader Save & Delete
// ------------------------

// bind ctrl+s
document.addEventListener('keydown', (event) => {
    const isCtrlOrCmd = event.ctrlKey || event.metaKey;
    if (isCtrlOrCmd && event.key.toLowerCase() === 's') {
      event.preventDefault();
      saveShader();
    }
  });

 var isPlayableCheckbox = document.getElementById('is_playable');
 isPlayableCheckbox.addEventListener('change', function(event) {
     if (isPlayableCheckbox.checked) {
         if (!bUserShaderCompiles) {
             alert('Your shader needs to compile before it can run on the tree.');
             isPlayableCheckbox.checked = false;
         }
     }
 });

const saveButton = document.getElementById('saveButton');
saveButton.addEventListener('click', saveShader);

const deleteButton = document.getElementById('deleteButton');
if( deleteButton )
    deleteButton.addEventListener('click', deleteShader);

// Function to save the shader code
function saveShader() {
    const shaderTitle =  document.getElementById('shaderTitle').value;
    const shaderAuthor =  document.getElementById('shaderAuthor').value;
    const isPlayable = document.getElementById("is_playable").checked;
    const shaderText = editor.getValue();

    // try to compile the user's shader right now
    setupTransformFeedbackProgram();

    if( !bUserShaderCompiles )
    {
        if( isPlayable )
        {
            let userConf = confirm("Your shader doesn't compile.\nYou can save it, but it won't run on the tree.\n\nSave anyway?")
            if(!userConf) return;

            document.getElementById("is_playable").checked = false;
        }
    }

    // Prepare the data to send
    const data = {
        shaderid: shaderIdent,
        title: shaderTitle,
        author: shaderAuthor,
        playable: document.getElementById("is_playable").checked,
        shadertext: shaderText
    };
    const statusElement = document.getElementById('saveStatus');
    
    // Send the data to the server
    fetch('/api/save', { // Replace with your actual endpoint
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            'X-CSRFToken': csrfToken
        },
        body: JSON.stringify(data)
    })
    .then(async response => {
        const text = await response.text();
        if (response.ok) {
            isDirty = false;
            const jsonResponse = JSON.parse(text);
            if (jsonResponse.redirect_url) {
                window.location.href = jsonResponse.redirect_url;
            } else {
                // Display "Saved!" message
                statusElement.textContent = 'Saved!';
                statusElement.style.color = 'green'; // Optional: Green color for success

                // Clear the message after 1 second (1000 milliseconds)
                setTimeout(() => {
                    statusElement.textContent = '';
                }, 1000);
            }
        } else {
            alert('Failed to save shader: ' + text);
        }
    })
    .catch(error => {
        console.error('Error:', error);
        alert('An error occurred while saving the shader: ' + error.message);
    });
}

function deleteShader() {
    const userConfirmed = confirm("Are you sure you want to delete this shader?")

    if(!userConfirmed)
        return

    const data = {
        shaderid: shaderIdent
    }

    fetch('/api/delete', {
        method: 'DELETE',
        headers: {
            'Content-Type': 'application/json',
            'X-CSRFToken': csrfToken
        },
        body: JSON.stringify(data)
    })
    .then(async response => {
        const text = await response.text();
        if (response.ok) {
            const jsonResponse = JSON.parse(text);
            if (jsonResponse.redirect_url) {
                window.location.href = jsonResponse.redirect_url;
            }
        } else {
            const jsonResponse = JSON.parse(text);
            let msg = "";
            if(jsonResponse['message'])
                msg = jsonResponse['message'];
            alert("Deletion failed:" + msg);
        }
    })
    .catch(error => {
        console.error("Error:", error);
        alert("Error deleting shader");
    });
}

// ------------------------
// Initial Setup
// ------------------------

window.addEventListener('beforeunload', function (e) {
    if (isDirty) {
      // Most browsers will display a generic message instead of the custom one
      const confirmationMessage = 'You have unsaved changes. Are you sure you want to leave?';

      (e || window.event).returnValue = confirmationMessage; // For legacy browsers
      return confirmationMessage; // For modern browsers
    }
    // No return value means no prompt
});

// Compile the initial user shader
setupTransformFeedbackProgram();

// Listen for changes in the editor to recompile shader
function debounce(func, wait) {
    let timeout;
    return function(...args) {
        clearTimeout(timeout);
        timeout = setTimeout(() => func.apply(this, args), wait);
    };
}

// only compile after the user has stopped typing for a bit
const debouncedSetupXformFeedback = debounce(setupTransformFeedbackProgram, 100);
editor.session.on('change', debouncedSetupXformFeedback);
editor.session.on('change', () => { isDirty = true;});
// Start the rendering loop
render();
