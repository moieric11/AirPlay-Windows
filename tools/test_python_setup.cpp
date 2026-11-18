#include "video/face_swap_process.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs=std::filesystem;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static ap::video::FaceSwapPythonStatus finish(ap::video::FaceSwapPythonSetup& setup) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(setup.status().active && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto result=setup.status();require(!result.active,"installer timed out");return result;
}
int main() {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root=fs::current_path()/fs::path(L"python-setup-test-\u00e9-"+std::to_wstring(stamp));
    struct Cleanup {
        fs::path root;
        ~Cleanup(){std::error_code ec;const auto target=fs::weakly_canonical(root,ec);
            if(!ec && target.parent_path()==fs::weakly_canonical(fs::current_path()) &&
                target.filename().u8string().rfind("python-setup-test-",0)==0)fs::remove_all(target,ec);}
    } cleanup{root};
    try {
        const auto engine=root/"face_swap"/"engine";
        fs::create_directories(engine);
        std::ofstream script(engine/"prepare_face_swap_python.ps1");
        script<<R"PS(param([string]$Backend,[string]$ProgressFile,[string]$Workspace,[switch]$TensorRT)
$ErrorActionPreference='Stop'
$utf8=New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path (Split-Path $Workspace) 'started.txt'),'started')
if ($Backend -eq 'directml') { Start-Sleep -Seconds 60; exit 1 }
if ($Backend -eq 'cuda') {
    [IO.File]::WriteAllText($ProgressFile,"failed`n0 0`nFixture installation error`n`n",$utf8)
    exit 1
}
$folder=Join-Path (Split-Path $Workspace) 'python-cpu'
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$python=Join-Path $folder 'python.exe'
[IO.File]::WriteAllText($python,'fixture; not executable')
[IO.File]::WriteAllText($ProgressFile,"complete`n0 0`nPython environment ready.`n$python`n",$utf8)
Write-Output 'Fixture installation completed'
)PS";
        script.close();
        ap::video::FaceSwapRuntime runtime;
        runtime.worker=(engine/"face_swap_worker.py").u8string();
        runtime.logs=(root/"face_swap"/"logs").u8string();
        ap::video::FaceSwapPythonSetup setup;
        setup.start(runtime,2);
        const auto success=finish(setup);
        require(success.complete && !success.failed,"completion state failed");
        require(fs::is_regular_file(fs::u8path(success.python)),"result path lost (Unicode or quoting)");
        require(fs::is_regular_file(fs::u8path(success.log)),"installer log missing");
        setup.start(runtime,0);
        const auto failure=finish(setup);
        require(failure.failed && !failure.complete && failure.message=="Fixture installation error","failure state lost");
        fs::remove(root/"face_swap"/"started.txt");
        setup.start(runtime,1);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!fs::exists(root/"face_swap"/"started.txt") && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(fs::exists(root/"face_swap"/"started.txt"),"cancellation fixture never started");
        const auto before=std::chrono::steady_clock::now();setup.cancel();
        require(std::chrono::steady_clock::now()-before<std::chrono::seconds(5),"cancellation blocked");
        const auto cancelled=setup.status();
        require(!cancelled.active && !cancelled.failed && !cancelled.complete && cancelled.phase=="cancelled","cancellation state failed");
        for(const auto& entry:fs::directory_iterator(root/"face_swap"))
            require(entry.path().filename().u8string().rfind(".python-setup-",0)!=0,"temporary workspace leaked");
        require(fs::is_regular_file(fs::u8path(success.python)),"retry or cancellation deleted an installed runtime");
        setup.start(runtime,99);require(finish(setup).failed,"invalid backend accepted");
        std::cout<<"Python setup: completion, errors, cancellation, Unicode paths and cleanup passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
