param(
    [ValidateSet("build", "clean", "rebuild")]
    [string]$Target = "build"
)

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$env:PATH = [System.Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("PATH", "User")

function Invoke-Build {
    cmd /c "`"$vcvars`" && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release"
}

function Invoke-Clean {
    if (Test-Path build) {
        Remove-Item -Recurse -Force build
        Write-Host "Cleaned."
    }
}

switch ($Target) {
    "build"   { Invoke-Build }
    "clean"   { Invoke-Clean }
    "rebuild" { Invoke-Clean; Invoke-Build }
}
