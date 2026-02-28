const Max = require('max-api');
const { spawn } = require('child_process');
const path = require('path');

// Python command: uses PYTHON_CMD env var if set, otherwise falls back to local venv
// 設定 Python 指令：優先使用環境變數 PYTHON_CMD，否則使用本地 venv
const PYTHON_CMD = process.env.PYTHON_CMD || path.join(__dirname, 'venv', 'bin', 'python');
const SCRIPT_NAME = 'cv_engine_optimized.py';

let pyProcess = null;

function startPython() {
    if (pyProcess) {
        Max.post("Python 已經在執行中。");
        return;
    }

    const scriptPath = path.join(__dirname, SCRIPT_NAME);
    Max.post(`正在啟動 Python: ${SCRIPT_NAME} ...`);

    // 啟動子程序
    pyProcess = spawn(PYTHON_CMD, [scriptPath]);

    // 監聽標準輸出 (正常訊息)
    pyProcess.stdout.on('data', (data) => {
        const msg = data.toString().trim();
        if (msg) Max.post(`[PY] ${msg}`);
    });

    // 監聽錯誤輸出 (⚠️ 這裡已修正，不會再崩潰了)
    pyProcess.stderr.on('data', (data) => {
        Max.post(`[PY ERR] ${data.toString()}`);
    });

    // 當 Python 結束
    pyProcess.on('close', (code) => {
        Max.post(`Python 程序已結束 (Code: ${code})`);
        pyProcess = null;
        Max.outlet('status', 0);
    });

    Max.outlet('status', 1);
}

function stopPython() {
    if (pyProcess) {
        pyProcess.kill();
        pyProcess = null;
        Max.post("已停止 Python 程序。");
        Max.outlet('status', 0);
    }
}

// Max 訊息接口
Max.addHandler('bang', startPython);
Max.addHandler('start', startPython);
Max.addHandler('stop', stopPython);

process.on('exit', stopPython);

// 一載入就自動執行
startPython();