Unicode true
SetCompressor /SOLID lzma
SilentInstall silent
RequestExecutionLevel user

!ifndef SOURCE_DIR
    !error "SOURCE_DIR is required"
!endif
!ifndef OUTPUT_FILE
    !error "OUTPUT_FILE is required"
!endif

!define APP_EXE "Track-N-Race - Qt.exe"

Name "Track-N-Race - Qt - Portable"
OutFile "${OUTPUT_FILE}"

Section
    InitPluginsDir
    SetOutPath "$PLUGINSDIR\app"
    File /r "${SOURCE_DIR}\*.*"
    ExecWait '"$PLUGINSDIR\app\${APP_EXE}"'
    SetOutPath "$TEMP"
SectionEnd
