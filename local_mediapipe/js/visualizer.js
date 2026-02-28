const videoElement = document.getElementById('video-bg');
const canvasElement = document.getElementById('canvas');
const canvasCtx = canvasElement.getContext('2d');
const loadingDiv = document.getElementById('loading');

// --- State Management ---
const state = {
    rawX: 0.5, rawY: 0.5,
    smoothX: 0.5, smoothY: 0.5,
    vx: 0, vy: 0,
    smoothVx: 0, smoothVy: 0,
    trail: [],
    lastTime: Date.now()
};

let width, height;
let flipY = true;

// Parameters
const SMOOTHING_FACTOR = 0.6; // Position smoothing (Higher = Faster)
const VELOCITY_ALPHA = 0.6;   // Velocity smoothing
const GRID_SIZE = 50;
const TRAIL_LENGTH = 30;

// --- Offscreen Canvas for Grid ---
const gridCanvas = document.createElement('canvas');
const gridCtx = gridCanvas.getContext('2d');
let gridCached = false;

function resize() {
    width = window.innerWidth;
    height = window.innerHeight;
    canvasElement.width = width;
    canvasElement.height = height;

    // Resize grid canvas
    gridCanvas.width = width;
    gridCanvas.height = height;
    gridCached = false; // Invalidate cache
}
window.addEventListener('resize', resize);
resize();

// --- UI Controls ---
const opacitySlider = document.getElementById('opacity-slider');
opacitySlider.addEventListener('input', (e) => {
    videoElement.style.opacity = e.target.value;
});

const flipCheck = document.getElementById('flip-check');
function updateVideoFlip() {
    videoElement.style.transform = `scaleX(-1) ${flipY ? 'scaleY(-1)' : ''}`;
}
flipY = flipCheck.checked;
updateVideoFlip();
flipCheck.addEventListener('change', (e) => {
    flipY = e.target.checked;
    updateVideoFlip();
});

// --- MediaPipe Hands Setup ---
const hands = new Hands({
    locateFile: (file) => {
        return `https://cdn.jsdelivr.net/npm/@mediapipe/hands/${file}`;
    }
});

hands.setOptions({
    maxNumHands: 1,
    modelComplexity: 1,
    minDetectionConfidence: 0.5,
    minTrackingConfidence: 0.5
});

hands.onResults(onResults);

// --- Tracking Logic ---
function onResults(results) {
    loadingDiv.style.display = 'none';

    // Clear canvas transparently
    canvasCtx.clearRect(0, 0, width, height);

    // Draw cached grid
    if (!gridCached) {
        drawGridToCache();
        gridCached = true;
    }
    canvasCtx.drawImage(gridCanvas, 0, 0);

    if (results.multiHandLandmarks && results.multiHandLandmarks.length > 0) {
        const landmarks = results.multiHandLandmarks[0];
        const indexTip = landmarks[8]; // Index finger tip

        // Get Raw Normalized Coordinates (0.0 - 1.0)
        // MediaPipe X is normalized 0-1 (Left-Right) of the original image.
        // Since we mirror the video with CSS (scaleX(-1)), we must also mirror the X coordinate 
        // so the dot appears over the hand in the mirrored view.
        let rawX = 1.0 - indexTip.x;
        const rawY = indexTip.y;

        // Smooth Position
        state.smoothX += SMOOTHING_FACTOR * (rawX - state.smoothX);
        state.smoothY += SMOOTHING_FACTOR * (rawY - state.smoothY);

        // Calculate Velocity
        const now = Date.now();
        const dt = (now - state.lastTime) / 1000;
        state.lastTime = now;

        // Better Velocity Calculation
        // Use delta of smoothed position (scaled to pixels later for visual, but normalized for output)
        // To avoid "teleport" spikes, only calculate if we have history.

        // Let's check history
        // (Using a temp var for prev position is cleaner than accessing trail)
        if (typeof state.prevX !== 'undefined') {
            const dx = state.smoothX - state.prevX;
            const dy = state.smoothY - state.prevY;

            if (dt > 0.001) {
                const instantVx = dx / dt;
                const instantVy = dy / dt;
                state.smoothVx += VELOCITY_ALPHA * (instantVx - state.smoothVx);
                state.smoothVy += VELOCITY_ALPHA * (instantVy - state.smoothVy);
            }
        }
        state.prevX = state.smoothX;
        state.prevY = state.smoothY;

        // --- Rendering ---
        renderVisuals(state.smoothX, state.smoothY, state.smoothVx, state.smoothVy);

        // --- Output to Max ---
        // Send normalized x, y, vx, vy, presence (1)
        const presence = 1;
        updateInfo(state.smoothX, state.smoothY, state.smoothVx, state.smoothVy, presence);
        if (window.max && window.max.outlet) {
            window.max.outlet(state.smoothX, state.smoothY, state.smoothVx, state.smoothVy, presence);
        }

    } else {
        // Hand Lost - Force velocity to 0 immediately
        state.vx = 0;
        state.vy = 0;
        state.smoothVx = 0;
        state.smoothVy = 0;
        
        // Preserve last known X/Y or reset? 
        // User asked for "velocity not high", implies stopping. 
        // We keep last position but zero velocity.
        
        const presence = 0;
        updateInfo(state.smoothX, state.smoothY, state.smoothVx, state.smoothVy, presence);
        if (window.max && window.max.outlet) {
            window.max.outlet(state.smoothX, state.smoothY, state.smoothVx, state.smoothVy, presence);
        }
    }
}

function renderVisuals(normX, normY, vx, vy) {
    // Convert to Screen Coordinates
    const screenX = normX * width;
    let screenY = normY * height; // Default

    if (flipY) {
        screenY = (1.0 - normY) * height; // Invert Screen Y
    }

    // Update Trail
    state.trail.push({ x: screenX, y: screenY });
    if (state.trail.length > TRAIL_LENGTH) state.trail.shift();

    // Draw Trail
    if (state.trail.length > 1) {
        canvasCtx.beginPath();
        canvasCtx.moveTo(state.trail[0].x, state.trail[0].y);
        for (let i = 1; i < state.trail.length; i++) {
            canvasCtx.lineTo(state.trail[i].x, state.trail[i].y);
        }
        const gradient = canvasCtx.createLinearGradient(state.trail[0].x, state.trail[0].y, state.trail[state.trail.length - 1].x, state.trail[state.trail.length - 1].y);
        gradient.addColorStop(0, 'rgba(0, 255, 255, 0)');
        gradient.addColorStop(1, 'rgba(0, 255, 255, 0.8)');
        canvasCtx.strokeStyle = gradient;
        canvasCtx.lineWidth = 4;
        canvasCtx.lineCap = 'round';
        canvasCtx.lineJoin = 'round';
        canvasCtx.stroke();
    }

    // Draw Halo Cursor
    canvasCtx.shadowBlur = 30;
    canvasCtx.shadowColor = 'rgba(0, 255, 255, 0.8)';
    canvasCtx.beginPath();
    canvasCtx.arc(screenX, screenY, 15, 0, Math.PI * 2);
    canvasCtx.fillStyle = 'rgba(0, 255, 255, 0.3)';
    canvasCtx.fill();

    canvasCtx.shadowBlur = 10;
    canvasCtx.shadowColor = '#fff';
    canvasCtx.beginPath();
    canvasCtx.arc(screenX, screenY, 6, 0, Math.PI * 2);
    canvasCtx.fillStyle = '#fff';
    canvasCtx.fill();
    canvasCtx.shadowBlur = 0;

    // Draw Velocity Vector
    const velMag = Math.hypot(vx, vy);
    if (velMag > 0.1) {
        const vecScale = 20.0;
        canvasCtx.beginPath();
        canvasCtx.moveTo(screenX, screenY);
        // Visual invert for velocity if flipped
        const vyVisual = flipY ? -vy : vy;
        canvasCtx.lineTo(screenX + vx * vecScale, screenY + vyVisual * vecScale);
        canvasCtx.strokeStyle = 'rgba(255, 0, 255, 0.8)';
        canvasCtx.lineWidth = 2;
        canvasCtx.stroke();
    }
}

function drawGridToCache() {
    gridCtx.clearRect(0, 0, width, height); // Clear cache
    gridCtx.strokeStyle = '#1a1a1a';
    gridCtx.lineWidth = 1;
    for (let x = 0; x < width; x += GRID_SIZE) {
        gridCtx.beginPath(); gridCtx.moveTo(x, 0); gridCtx.lineTo(x, height); gridCtx.stroke();
    }
    for (let y = 0; y < height; y += GRID_SIZE) {
        gridCtx.beginPath(); gridCtx.moveTo(0, y); gridCtx.lineTo(width, y); gridCtx.stroke();
    }
}

function updateInfo(x, y, vx, vy, presence) {
    document.getElementById('val-x').textContent = x.toFixed(2);
    document.getElementById('val-y').textContent = y.toFixed(2);
    document.getElementById('val-vx').textContent = vx.toFixed(2);
    document.getElementById('val-vy').textContent = vy.toFixed(2);
    document.getElementById('val-hand').textContent = presence;
}

// --- Camera Setup ---
const cameraSelect = document.getElementById('camera-select');
let currentStream = null;

async function getCameras() {
    try {
        const devices = await navigator.mediaDevices.enumerateDevices();
        const videoDevices = devices.filter(device => device.kind === 'videoinput');

        cameraSelect.innerHTML = '';
        videoDevices.forEach(device => {
            const option = document.createElement('option');
            option.value = device.deviceId;
            option.text = device.label || `Camera ${cameraSelect.length + 1}`;
            cameraSelect.appendChild(option);
        });

        if (videoDevices.length > 0) {
            startCamera(videoDevices[0].deviceId);
        }

        cameraSelect.onchange = () => {
            startCamera(cameraSelect.value);
        };
    } catch (err) {
        console.error("Error listing cameras:", err);
    }
}

async function startCamera(deviceId) {
    if (currentStream) {
        currentStream.getTracks().forEach(track => track.stop());
    }

    const constraints = {
        video: {
            deviceId: deviceId ? { exact: deviceId } : undefined,
            width: 1280,
            height: 720
        }
    };

    // We need to pass the running video element to Camera util, 
    // but Camera util handles getUserMedia internally? 
    // Actually, MediaPipe Camera Utils `new Camera` takes the video element and handles the RAF loop,
    // but it relies on us setting the srcObject?
    // Wait, looking at documentation/examples, `new Camera` usually wraps logic. 
    // Let's stick to the MediaPipe Camera Utils pattern which is cleaner for their loop.
    // BUT MediaPipe Camera Utils doesn't easily support deviceId switching via its constructor options in a standard way
    // without re-instantiating.

    // Let's do it manually to ensure we get the right device
    try {
        const stream = await navigator.mediaDevices.getUserMedia(constraints);
        currentStream = stream;
        videoElement.srcObject = stream;
        // Wait for video to load then start processing
        videoElement.onloadedmetadata = () => {
            videoElement.play();
            // Just rely on MediaPipe Camera utility to drive the loop? 
            // No, `camera.start()` does everything.
            // But `camera.start()` inside MediaPipe Utils might override our stream if we are not careful.
            // Actually, MediaPipe Camera Utils is designed to TAKE a video element, and optionally a width/height.
            // It doesn't usually do getUserMedia with specific constraints for us easily.

            // Let's USE our own getUserMedia (above), and just use `hands.send` in a requestAnimationFrame loop 
            // instead of `new Camera(...)` which hides control. 
            // This gives us full control over device selection.

            requestAnimationFrame(processVideo);
        };
    } catch (err) {
        console.error("Camera start error:", err);
    }
}

async function processVideo() {
    if (!videoElement.paused && !videoElement.ended) {
        await hands.send({ image: videoElement });
    }
    requestAnimationFrame(processVideo);
}

// Initialize
getCameras();
