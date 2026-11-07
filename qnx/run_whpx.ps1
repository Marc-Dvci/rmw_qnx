# Boots the rmw_qnx guest under QEMU for Windows with the Windows Hypervisor Platform, headless. No window opens.
#
#   powershell -File qnx/run_whpx.ps1 [-Dir .guest/vm] [-Qemu PATH] [-Stop]
#
# QNX 8.0's startup needs the always-running APIC timer bit in CPUID leaf 6, which Hyper-V hides from its guests;
# "+arat,x-force-features=on" puts it back. AVX-512 is off. ssh is forwarded to 127.0.0.1:2232, the web port to
# 127.0.0.1:8091 and the serial console to 127.0.0.1:5510 (also logged to console-<time>.log).
# -Stop stops this guest only, by the PID in its pidfile.
param(
    [string]$Dir = (Join-Path $PSScriptRoot "..\.guest\vm"),
    [string]$Qemu = "D:\tools\qemu\qemu-system-x86_64.exe",
    [switch]$Stop
)
$Dir = (Resolve-Path $Dir).Path
$pidFile = Join-Path $Dir "qemu.pid"
if ($Stop) {
    if (Test-Path $pidFile) {
        $id = [int](Get-Content $pidFile -Raw).Trim()
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -like "qemu-system*") { Stop-Process -Id $id -Force; "stopped pid $id" }
        else { "no guest running under pid $id" }
    } else { "no pidfile" }
    return
}
if (Test-Path $pidFile) {
    $id = [int](Get-Content $pidFile -Raw).Trim()
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -like "qemu-system*") { "already running as pid $id"; return }
}
$noAvx512 = "-avx512f,-avx512dq,-avx512ifma,-avx512cd,-avx512bw,-avx512vl,-avx512vbmi,-avx512vbmi2,-avx512vnni,-avx512bitalg,-avx512-vpopcntdq,-avx512-bf16"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$a = @("-accel", "whpx", "-machine", "pc", "-cpu", "host,+arat,x-force-features=on,$noAvx512", "-smp", "2", "-m", "4G",
    "-drive", "file=disk-qemu.vmdk,if=ide,id=drv0",
    "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:2232-:22,hostfwd=tcp:127.0.0.1:8091-:8080",
    "-device", "virtio-net-pci,netdev=net0", "-object", "rng-builtin,id=rng0", "-device", "virtio-rng-pci,rng=rng0",
    "-kernel", "ifs.bin", "-display", "none", "-vga", "none",
    "-chardev", "socket,id=s0,host=127.0.0.1,port=5510,server=on,wait=off,logfile=console-$stamp.log", "-serial", "chardev:s0",
    "-pidfile", "qemu.pid")
$p = Start-Process -FilePath $Qemu -WorkingDirectory $Dir -ArgumentList $a -PassThru -WindowStyle Hidden `
    -RedirectStandardError (Join-Path $Dir "qemu-$stamp.err") -RedirectStandardOutput (Join-Path $Dir "qemu-$stamp.out")
"started pid $($p.Id); ssh 127.0.0.1:2232, console 127.0.0.1:5510, log console-$stamp.log"
