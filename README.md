﻿# NZBDrive
Mount NZB files as drives or folders in Linux or Windows (under construction!).

## Dependencies
* C++11
* Tinyxml2
* Libfuse on Linux
* Dokanx on Windows
* Openssl
* Boost libraries:
  * thread
  * filesystem
  * system

## How to compile on Linux
Install prerequisites and run `make` in the src-directory. This should generate an executable `nzbmounter`.

## How to compile on Windows

Download and install Visual Studio Community from <https://www.visualstudio.com/downloads/>

Download and install SDK10 from <https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk>

Donwload and install WDK10 from <https://developer.microsoft.com/en-us/windows/hardware/windows-driver-kit>

Clone the Dokany library from <https://github.com/OleStauning/dokany.git> in the same directory as you cloned NZBDrive.

Enter the dokany directory and run bat-file `build.bat`. 

Read the wiki page <https://github.com/dokan-dev/dokany/wiki/Installation> about how to install dokany on your computer. 
If the dokany driver is signed by a test signature then you have to enable windows test mode:
```
bcdedit.exe -set loadoptions DDISABLE_INTEGRITY_CHECKS
bcdedit.exe -set TESTSIGNING ON
```
Then reboot.

Download either 32 and/or 64 bit boost binaries from <https://sourceforge.net/projects/boost/files/boost-binaries/1.63.0/>

* boost_1_63_0-msvc-14.0-64.exe
* boost_1_63_0-msvc-14.0-32.exe

Install boost in directory "boost" in the same directory as you cloned NZBDrive.

Download and install Perl from <http://www.activestate.com/activeperl/downloads>.

Download and install nasm from <http://www.nasm.us/pub/nasm/releasebuilds/2.12.02>

Download openssl-1.1.0e.tar.gz from <https://www.openssl.org/source/> extract and rename to the directory "openssl" in the same directory as you cloned NZBDrive.

Enter the "openssl" directory.

For compiling 32 bit libraries:

Set environment variables by running `C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\bin\vcvars32.bat`

Configure the build files by running `perl Configure VC-WIN32 --prefix=e:\openssl\x86`

Then build the openssl library by running `nmake clean` and `nmake install`

For compiling 64 bit libraries:

Set environment variables by running `C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\bin\amd64\vcvars64.bat`

Configure the build files by running `perl Configure VC-WIN64A --prefix=e:\openssl\x64`

Then build the openssl library by running `nmake clean` and `nmake install`

Clone tinyxml2 from <https://github.com/leethomason/tinyxml2.git> in the same directory as you cloned NZBDrive.

Download the Community Edition of the Extended WPF Toolkit from <http://wpftoolkit.codeplex.com/>, and unpack to in the same directory as you cloned NZBDrive.

Finally: open the solution file NZBDrive.sln in Visual Studio and Build.

You will now have a directory structure like this:

/
├── boost/
├── dokany/
├── Extended WPF Tooklit Binaries/
├── NZBDrive/
│   ├── Source/
│   │   ├── Linux/
│   │   ├── NZBDriveLib/
│   │   ├── Windows/
│   │   ├── x64
│   │   ├── x86
│   │   ├── Makefile
│   │   └── NZBDrive.sln
│   ├── License
│   └── README.md
├── openssl/
└── tinyxml2/

The binaries are located in x86/x64.

## Usage in Linux
Create an empty directory that you want to use as mount point.

`mkdir mnt`

Then you can mount an nzb-file with the command:

`nzbmounter mnt filename.nzb`

## TODO'S:
* Fix warnings
* Proper configure scripts.
* 




