@echo off
rem Compressor NEXT - Untersuchung auf einem Rechner, dessen Treiber eigene Listen von Effekt-Modulen
rem vorgibt (z. B. Realtek). Installiert die beiliegende Version, probiert dabei sechs Kombinationen der
rem Stream-Liste durch (der Ton setzt etwa eine halbe Minute lang mehrmals kurz aus), stellt danach den
rem funktionierenden Zustand wieder her und erzeugt den Diagnosebericht auf dem Desktop.
rem Setup, Diagnose und diese Datei muessen im selben Ordner liegen.
setlocal
set "CNX_SETUP="
for %%F in ("%~dp0CompressorNext-Setup-*.exe") do set "CNX_SETUP=%%~fF"
if not defined CNX_SETUP (
  echo Setup-Datei CompressorNext-Setup-*.exe fehlt im Ordner %~dp0
  pause
  exit /b 1
)
echo Compressor NEXT - Untersuchung
echo.
echo Bitte die Windows-Abfrage bestaetigen. Danach laeuft alles von selbst (etwa 1 Minute).
echo Der Ton setzt dabei mehrmals kurz aus.
echo.
"%CNX_SETUP%" /SILENT /probe=vendor
echo Setup beendet (Code %ERRORLEVEL%). Der Diagnosebericht wird erstellt ...
call "%~dp0CompressorNext-Diagnose.cmd"
endlocal
