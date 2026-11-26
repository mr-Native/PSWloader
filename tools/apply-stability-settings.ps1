# apply-stability-settings.ps1 v1.0 (08.11.2026) - PS5 native Windows: mandatory stability setting.
# Finding (HW, 08.11.): with Windows' default "dynamic tick" (tickless, the timer is programmed as a one-shot
# deadline) native Windows on the PS5 froze reproducibly ~537 s after kernel start (4/4 boots: clock stopped,
# network gone, no blue screen). With a periodic tick (disabledynamictick) Windows kept running > 900 s.
# Standard boot option, reversible with: bcdedit /deletevalue {current} disabledynamictick
# Belongs in every installation (AIO package / USB install), takes effect from the next boot.
$ErrorActionPreference = 'Stop'
bcdedit /set '{current}' disabledynamictick yes | Out-Null
(bcdedit /enum '{current}' | Select-String 'disabledynamictick') -join ''
