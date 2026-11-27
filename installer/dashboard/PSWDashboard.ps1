# PSWDashboard.ps1 v1.4 (PSWloader v1.2.1; v1.4: dynamic live refresh for Display/Monitor/Resolution, Network Wi-Fi/Ethernet, Memory, Storage/USB drives and Health via DisplaySettingsChanged + NetworkChange events + 5 s background poll; v1.3: starts at every sign-in (-Autostart from PSWSetup's Run entry; switch "Start with Windows" under Tools, per user); software rendering (psw-ui v1.1); v1.2: the window appears first, the data is read in the background) - PSWloader Dashboard: one window for everything PSWloader puts on a PC.
#   Overview  console, PSWloader/firmware/Windows, live graphs (processor, graphics, memory, temperature + fan), hardware,
#             storage - everything read on the PC (performance counters, CIM, PS5 thermal log, the last check)
#   Health    the PSWloader check (PSWSelfCheck -ChecksOnly): problems and notes; checked at every start and every 6 hours,
#             "Check now" on demand; Troubleshoot (EXPERIMENTAL, PSWFix.ps1): fresh check first, only offered for fixable problems
#   Tools     UEFI Boot Manager (restart straight into it), support zip (progress, folder, GitHub issue), What's new,
#             report a problem, support the project (display and Bluetooth: Windows Settings)
#   Updates   installed version, look for a new release, install it (the updater asks for administrator rights)
# Runs without administrator rights: the steps that need SYSTEM run as scheduled tasks that every user may start
# ("PSWloader Check", "PSWloader Support Zip", "PSWloader Boot Manager", "PSWloader Fix", set up by PSWSetup component Dashboard).
# Open source, part of PSWloader (https://github.com/mr-Native/PSWloader). -Preview <png> [-Page ...] renders a page with sample
# graphs; -SelfTest <png> opens the real window, lets it read live values for 8 s, saves a picture of it and closes (test).
param([string]$Preview = '', [ValidateSet('Overview', 'Power', 'Health', 'Tools', 'Updates')][string]$Page = 'Overview', [string]$SelfTest = '', [switch]$Autostart)
$ErrorActionPreference = 'SilentlyContinue'
if ($Autostart) {   # v1.3 (user 26.11.2026): at sign-in, unless this user turned it off under Tools; never on top of the welcome window
  if ((Get-ItemProperty -Path 'HKCU:\Software\PSWloader' -Name DashboardAutostart -ErrorAction SilentlyContinue).DashboardAutostart -eq 0) { exit 0 }
  Start-Sleep -Seconds 20
  $until = (Get-Date).AddMinutes(15)
  while ((Get-Date) -lt $until -and @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match 'psw-welcome\.ps1' }).Count) { Start-Sleep -Seconds 5 }
}
$Data = "$env:ProgramData\PSWloader"
$DashData = Join-Path $Data 'dashboard'
$ChecksFile = Join-Path $DashData 'checks.json'
$SupportProgress = Join-Path $DashData 'support-progress.txt'
$FixProgress = Join-Path $DashData 'fix-progress.txt'
$FixResult = Join-Path $DashData 'fix-result.json'
$Repo = 'mr-Native/PSWloader'
$Blue = '#3A86E0'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase

# >>> psw-ui v1.1 (PSWloader v1.2) - the look of the PSWloader windows (WPF, 800 wide, title, text, grey footer, buttons on
# the left, first button blue (PSWloader banner colours)). Source: installer/engine/psw-ui.ps1, copied between these markers into the scripts that show
# windows (tools/psw-display.ps1, tools/psw-uefi-bootmanager.ps1, installer/updater/PSWUpdateCheck.ps1) by tools/sync-psw-ui.py.
#   Show-PswDialog -Bar -Head -Text -One [-Two] [-Three] [-Stay @{ 1 = { ... } }]  -> 1, 2 or 3 (closing the window = the last button);
#     a button listed in -Stay only runs its script block (open a folder, a web page) and the window stays (user 26.11.2026:
#     a window must not disappear because something was opened from it - only its closing button ends it)
#   Show-PswChoice -Bar -Head -Text -Items -Selected -One -Two -> index of the chosen item, -1 = second button / closed
#   -Preview <png> on both: render instead of showing
# v1.3 (HW 26.11.2026: after a cold start the Dashboard window on the Realtek Windows stayed a transparent frame - WPF's
# hardware rendering goes through D3D9 of the PS5 GPU driver and presented nothing; software rendering drew it at once):
# every PSWloader window renders in software - independent of the GPU driver (broken driver, PS5 Pro without driver)
try { Add-Type -AssemblyName PresentationCore; [System.Windows.Media.RenderOptions]::ProcessRenderMode = [System.Windows.Interop.RenderMode]::SoftwareOnly } catch { }
function Set-PswDarkTitle($Window) {   # v1.2 (user 26.11.2026): PSWloader windows have no Windows title bar (PowerShell icon,
  # title text) - a frameless window with its own minimize/close buttons at the top right, moved by dragging any free area;
  # the PSWloader logo with an own identity in the taskbar; in front when it appears (Windows' foreground lock kept windows
  # of background tasks behind others - a short Alt key press lets SetForegroundWindow through)
  try { $pre = "$env:ProgramFiles\PSWloader\PswDwm.dll"   # compiled once by PSWSetup (component Dashboard) - saves about half a second of compiling at every start
        if (-not ('PswDwm.Native' -as [type]) -and (Test-Path -LiteralPath $pre)) { try { Add-Type -Path $pre } catch { } }
        if (-not ('PswDwm.Native' -as [type])) { Add-Type -Namespace PswDwm -Name Native -MemberDefinition @'
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(System.IntPtr h, int a, ref int v, int s);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c);
[DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, System.UIntPtr e);
[DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern int SetCurrentProcessExplicitAppUserModelID(string id);
'@ } } catch { }
  try { [void][PswDwm.Native]::SetCurrentProcessExplicitAppUserModelID('PSWloader.Windows')   # own taskbar entry, not PowerShell's
        if ($null -eq $Window.Icon) {
          $png = 'iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAYAAABXAvmHAAAMDklEQVR42t1af3BV5Zl+3vf7zr333B8JIRCw0NgEKoqLU5GlSMvGCkxr1e6Ukqwzu+7MTp11rJWxnaWuxhqoYmvtrFM73V233dmhnZ2xCWPX6Uor/QFUViIrLquCC0JQFDSQhCQ3995zz/m+790/zk1ASkJucDtbz8z95853znme93t/PO/7HUK1V0cHt165gbrayI7+tfyRtxeQ0CdF3LWA/JEIPkxwdQD8ypKSgE8T4S2AXiXi3UKy6/l75h4cfUZrp6iu/RsEGze6auBQNWtbO4VHgS976Ngc5am1gKwR55YoL5UGMcQZiDUQsYBI5U4CkQIpDWINiIONgiIxvwjQUzayW7rvbzw+RqSNHAB5/wh0CGMjOQBY8eAbTS6R+Arg/lIlc7ViQ7iwBCfWQSBEoPi5dM6zRQCICAQEYlLMCR+kErDl/BDAP+IwfOy5r3/k6LnvvCgCFYvYa/76Cc9vvvFrwrxeeelaGwzDiTUAEUH4dwFf6BIRkANEmJRWqRrYqDhEzj1a6nnm23v/6fZo9N1TJtDSsV3v3Pgps+wbRxapZOqHKpldaktDcGINAap60BORgWVSWvm1sOWRPbYc3Nb9wLxXRjFUTaClQ/TOjWSWbeppU57/z6x01gR5QyAFer+A/y4PgVidymlnzYiNSl/sbm/uHMUyaQKjNyzf1HOXSuUed1EAZyNLxAq/h0vEWVaeYi8FG+TXPd/e/L3xSNBE4HW67nETDFuII4AYv9dLHIhFp2qUKZ4elwSdL2Cv3XRkrU7VdtlywUIc/5+5zKTinJ1KZpQJhlp3t8/bcm5g85n6FOf4pZsOLWTP3+xM4ABHE4IXF/8mRlFZJ9UTICLAkTOBY8/fvHTToYVdbWQ7OoTPISB04MouuuaJFz1NyR+z8tLOhDK+2wggAvLSIC89muLPT5AUOJEFKW8SZM/Lgp0JhZWX1pT88TVPvOgduLKLAKExAq2d4K62Npvsq1/vZeoWmyBvxg9YAUiBvBTC3gMIew+AvBRA6r1WFgdKpCFRCaW39sAU+sCJ7JRIELEyQd54mbrFyb769V1tbba1M8ZO6BDGBsiKhw7PtTr1Ggg+nKHxczwBcDj1bAcKB38BAMgs+DQaPvMwBA5wsQogL43yiX04ufUemOET4GQO9dffi9yiL8AVBwDWVQcEWAsEJWWCK567f/7b2ADiFuxgEElE+h7t5zLijBsXvLPgZBb5V55Cft+TIC8J0knkX96CU9seAHv+GZIuQv/2byLqPwJOZODKIzi19V4UDv0SnJ4OOFP1PogzTvu5TET6HhBJC3YwAcDHNx2ZpVgfJlBGxGJCAunpOLX1bzH83z+B8msh1gCs4Yr9qF16G2as+jpcOAIXDOH4j9bChYVRVwZcBJDC7LU/gN+4FK40WOVOiBApCKRgnZn/Qvu8Xo4DQd+i/WlZJ9ZOKA8IgDPwGz8OsWEsABD/x+npGNrzQ5ze9Tg4kYPy65GYtTAGqTxALMAexEbo/be7UO49AE7VVLkTRE6s1f60LEPfciYLkWsVG8oFtQ0puPIIMld8FnWfuAuueBoYjXVnwX4dBp57DEMv/gvYr8WMlfcjcckiuGAotrRYkJeCC4bQ+9SXEA2+BUpkAWercyUbCsi1AoBa9vCx+QQ8CGc9QHgS9wMuQuajK+GCQQRv7o5BVLIL6SSKh38DnZuNzGWr4c9dgmLPTrhiP0inAGdAOglb7EPpzW5kF3wGnMzEOzGJekkAw1kSoGHuyq/+RDWu+vIanciusbbsaLLqUgCxZWTmr4IZehvl43vPkKg0L4XDv4ZX14R00wokL7kKxdd/CVceAakkIAbkpWGG30ZwYh+yl98Y1wlnJ0VC4JxOZDy40itqzvVfvVN5iaudDR1NVu8QVWqZReay1SifPIjw5H5QIlPxdQWIQ/Hwr5GcdSXSzSuQmLkAhYO/gNio0pUZkJdB1H8YYd8hZK+46Uzfc2ESjnWSbWT6VOOqu9uJ1Zw4mKrQPERxsSJG5qOrERzfi2igB+RlYnDsQUyA0pEdSM1djHTTn8CruxSFgz+PBRhzpdhlEPbuhxk6gezlN0KcqVT1iXMJsSI4E6m519/9IBGycA5VizYiwFmQTiIzfyWKR3fB5E+APD8moZNw5WGU3vgPpBqXIt20AjrbEJNgHUMRB0pkUT7+ElwwjMxlqyAmmtiWEuskcU6pD6+8+yEAusoG/70kbARO1SDd3ILi67+CKw6AdDLOOtqHHTmJ8vGXkG5uQbpxGTiVQ+Hgs/GaMdmRRemNXSCVRLrpkxBTGl+KjRpaRDMRUlMTWWddrCBhAbrmQ5i99gfQNR+C2LBSvAw4VYvyOy+jdPQ5iCmjdvGtqF95H8QEZ0CKBSdzGH65C7bYD7A38WBCHIiQep+bFImtOmHjFvv3mPUvYs4DACyC4KKbLWdBng9b6MM7XbchGjwGUok4rbKCC4aRnL0I6aYVIJ3C8Mud6Nu2Ma4Lo7tPCq6cR+6qtVDp+lh2TESHGCIIGMAgMeOMLphC56eTcFER7/70TkQDR8HJXCWdakhYhK6ZjYabHoWuuxT5Az9D37MPxMWr4so0qqWu/nNMu/YOuDAfy/MJOjViBoBBJsIxYg0QZCrgwRoigt6n16F8Yh84VVupqgpiyuBkFrM+912k5i5B4dA2nNr6tbhokRq73xYHkLn8s5jx6W9AomAy4zgh1iDCMXaCV4kVBFUSkLhhI5XAqWfWo9TzW7BfN6Y44QyIGA03/x3SzStQPLoLJ3/2lTjPV3QR2IMrnYbf9Ak03PQdiI3i/y8QCQIIsYITvMrMdnelk6JqghUQcCKLvm0dGHnt38Hp+orl4wIlNsLMG76JzIIbEBzfh96n18EF+YrfV8AHQ0hechVm/enjoErGmmQ8EkTAbHezE2+HCYsBk+Jq8HN6Ovp3fBvD//WvZzUosQ1cWMCM1R2o+difIeo7hN6n74LNvwtKpON1rCHlEXjTP4LZn/97cDIHMeVJT26YFJuwGDjxdnD3fY2HAdnLCV8EsJNxHfJ8DOx4FIPd/1hxGzuWGVxpCNNb/ga1f/xXKPcewLs/vRNh3xFQMlexsIZEJajMDMxe8w9QuVmQsHiB1Pse21lO+ALI3u77Gg/HlIW7SCXogrMPiVvKwv9sxcBvHwOnpp1Jg6zgSgOYtvwO1F17B1wwhL5fPYjg+EtQfu1YbIgtgz0fsz7/fSTq50PK+UmDH+vKVIIg3DXW0DiYJ01pcIRJqQlJCADWKB17AaQTID47DQ6gZvFfoL5lPVxYgC32I+w9AOXXxcFJqrJThIbPfRepOVfDjjY6VYBnUsqUBkcczJMAwC0d2/UL7fN6nXWblV9DF3QjcUjMvAwSFiHiAGKYkZPILrwZM1ZviHtgZ6D8OuiaS+CCIRBriA3hTAkNNz6C9Lzr4EqnK4KuqtRhlV9DzrrNL7TP623p2K55J65zECFPzCOmlC8Qax53FzhuKXOL1iD3sVsgURliy8gt+gJm3vCtWP+MuZSH+k/dC69+HlxUBCcyaLjhW8guvBmu2D+lsQqxZlPKFzwxj0CEduI6R2fPRJdvOnqfl63fFI70GRrXPJXBlvIQ9r4GAEjMuiJ2k7M7qspgy5WGEPa9Dl07F17tHLhyfkpzYhFnEtkZOhrpb3++venhUcyjb6PWzi7uOd3Myf6Z3SqZXmyCvJ1wOieIdT8AiUqVDErnrdSsU/EUY1ShTmHcrlM5ZcvFl8r1p5Y11/W4rrZWB9BoE0+ycH+r7L19SWSkfKuzUZF1gsafA8aDO4mKkKhYsTqdV3BBLFw4UglknpLYYp0gZ6OikfKte29fEi3c3yoAyQdrvA4AXW1kWzpE726ft8UGw+u0X6PA7HDRHc8UZS6z036NssHwut3t87a0dIg+99Dvg3fE9Id2yDduVO3cSKalY7vubm/utEFxuTPhHi9Tr4kVCcRM7chlImcXQ6zIy9RrZ8I9Niguj8FvHxc8AEzoEm/u3OxaO0Vt+9L0dxuaF2zWuUvLUOoanaxJi41IYnUmBKGLOOh2TEppfxqL2CFE5YeCnme++J/fWf1Oa6eorV9umvpB9wfiU4Oz1/7hfuzxnt34//W5zf8CYLHGjH9Kfw4AAAAASUVORK5CYII='
          $Window.Icon = [System.Windows.Media.Imaging.BitmapFrame]::Create((New-Object IO.MemoryStream(, [Convert]::FromBase64String($png))), [System.Windows.Media.Imaging.BitmapCreateOptions]::None, [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad) } } catch { }
  try {
    $min = ($Window.ResizeMode -ne 'NoResize')
    $Window.WindowStyle = 'None'; $Window.ResizeMode = $(if ($min) { 'CanMinimize' } else { 'NoResize' })
    $inner = $Window.Content; $Window.Content = $null
    $g = New-Object System.Windows.Controls.Grid; [void]$g.Children.Add($inner)
    $bar = New-Object System.Windows.Controls.StackPanel; $bar.Orientation = 'Horizontal'; $bar.HorizontalAlignment = 'Right'; $bar.VerticalAlignment = 'Top'
    foreach ($d in @(@('min', 0xE921), @('close', 0xE8BB))) {
      if ($d[0] -eq 'min' -and -not $min) { continue }
      $b = New-Object System.Windows.Controls.Border; $b.Width = 46; $b.Height = 32; $b.Background = [System.Windows.Media.Brushes]::Transparent; $b.Tag = $d[0]
      $t = New-Object System.Windows.Controls.TextBlock; $t.Text = [string][char]$d[1]; $t.FontFamily = 'Segoe MDL2 Assets'; $t.FontSize = 10
      $t.Foreground = [System.Windows.Media.Brushes]::Gainsboro; $t.HorizontalAlignment = 'Center'; $t.VerticalAlignment = 'Center'; $b.Child = $t
      $b.Add_MouseEnter({ $this.Background = (New-Object System.Windows.Media.BrushConverter).ConvertFromString($(if ($this.Tag -eq 'close') { '#C42B1C' } else { '#353535' })) })
      $b.Add_MouseLeave({ $this.Background = [System.Windows.Media.Brushes]::Transparent })
      $b.Add_MouseLeftButtonDown({ $_.Handled = $true })
      $b.Add_MouseLeftButtonUp({ $pw = [System.Windows.Window]::GetWindow($this); if ($this.Tag -eq 'min') { $pw.WindowState = 'Minimized' } else { $pw.Close() } })
      [void]$bar.Children.Add($b) }
    [void]$g.Children.Add($bar); $Window.Content = $g
    $Window.Add_MouseLeftButtonDown({ try { $this.DragMove() } catch { } })
    $h = (New-Object System.Windows.Interop.WindowInteropHelper $Window).EnsureHandle()
    $v = 1; [void][PswDwm.Native]::DwmSetWindowAttribute($h, 20, [ref]$v, 4)            # dark frame
    $v = 2; [void][PswDwm.Native]::DwmSetWindowAttribute($h, 33, [ref]$v, 4)            # Windows 11: rounded corners (with its shadow)
    $v = 0x00383838; [void][PswDwm.Native]::DwmSetWindowAttribute($h, 34, [ref]$v, 4)   # Windows 11: thin grey edge
  } catch { }
  $Window.Add_ContentRendered({
    try { $h = (New-Object System.Windows.Interop.WindowInteropHelper $this).Handle
          [PswDwm.Native]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [PswDwm.Native]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
          [void][PswDwm.Native]::ShowWindow($h, 9); [void][PswDwm.Native]::SetForegroundWindow($h); [void][PswDwm.Native]::BringWindowToTop($h) } catch { }
    $this.Activate() | Out-Null })
}
function New-PswUiRoot([string]$Head, [string]$Text, [string[]]$Buttons, [string]$Middle = '') {
  Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
  $e = { param($t) [Security.SecurityElement]::Escape($t) -replace "`n", '&#x0a;' }
  $b = ''; for ($i = 0; $i -lt $Buttons.Count; $i++) {
    $style = if ($i -eq 0) { 'Background="#3A86E0" Foreground="#FFFFFF" BorderBrush="#3A86E0" IsDefault="True"' } else { 'Background="#262626" Foreground="#F2F2F0" BorderBrush="#454545" Margin="12,0,0,0" MinWidth="110"' }
    if ($i -eq $Buttons.Count - 1 -and $Buttons.Count -gt 1) { $style += ' IsCancel="True"' }
    $b += "<Button x:Name=""B$($i + 1)"" Style=""{StaticResource Btn}"" Content=""$(& $e $Buttons[$i])"" BorderThickness=""1"" $style/>"
  }
  [Windows.Markup.XamlReader]::Parse(@"
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Background="#1B1B1B" Width="800" TextOptions.TextFormattingMode="Display" UseLayoutRounding="True" SnapsToDevicePixels="True">
  <Grid.Resources>
    <Style x:Key="Btn" TargetType="Button">
      <Setter Property="FontFamily" Value="Segoe UI Semibold"/><Setter Property="FontSize" Value="14"/>
      <Setter Property="Height" Value="40"/><Setter Property="Cursor" Value="Hand"/><Setter Property="Padding" Value="22,0"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="B" CornerRadius="8" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}"
                    BorderThickness="{TemplateBinding BorderThickness}" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="B" Property="Opacity" Value="0.88"/></Trigger>
              <Trigger Property="IsPressed" Value="True"><Setter TargetName="B" Property="Opacity" Value="0.75"/></Trigger>
              <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="B" Property="BorderBrush" Value="#F2F2F0"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="RadioButton">
      <Setter Property="FontFamily" Value="Segoe UI"/><Setter Property="FontSize" Value="14"/><Setter Property="Margin" Value="0,5,0,5"/>
      <Setter Property="Foreground" Value="#F2F2F0"/><Setter Property="Cursor" Value="Hand"/><Setter Property="VerticalContentAlignment" Value="Center"/><Setter Property="Padding" Value="10,0,0,0"/>
    </Style>
  </Grid.Resources>
  <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
  <StackPanel Grid.Row="0" Margin="36,30,36,28">
    <StackPanel Orientation="Horizontal" Margin="0,0,0,18"><Grid Width="30" Height="30"><Ellipse Fill="#3A86E0"/><Path Data="M10,9 L15.5,15 L10,21 M16,9 L21.5,15 L16,21" Stroke="#1B1B1B" StrokeThickness="3.2" StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round"/></Grid><TextBlock Margin="10,0,0,0" VerticalAlignment="Center" FontFamily="Segoe UI" FontWeight="Bold" FontSize="20"><Run Text="PSW" Foreground="#F2F2F0"/><Run Text="loader" Foreground="#3A86E0"/></TextBlock></StackPanel>
    <TextBlock Text="$(& $e $Head)" FontFamily="Segoe UI Semibold" FontSize="24" Foreground="#F2F2F0" Margin="0,0,0,16" TextWrapping="Wrap"/>
    <TextBlock Text="$(& $e $Text)" FontFamily="Segoe UI" FontSize="14" LineHeight="21" Foreground="#DADAD5" TextWrapping="Wrap"/>
    $Middle
  </StackPanel>
  <Border Grid.Row="1" Background="#151515" BorderBrush="#2C2C2C" BorderThickness="0,1,0,0" Padding="36,18,36,20">
    <StackPanel Orientation="Horizontal">$b</StackPanel>
  </Border>
</Grid>
"@)
}
function Show-PswUiRoot($Root, [string]$Bar, [string]$Preview) {
  if ($Preview) {
    $Root.Measure([System.Windows.Size]::new([double]::PositiveInfinity, [double]::PositiveInfinity)); $Root.Arrange([System.Windows.Rect]::new($Root.DesiredSize)); $Root.UpdateLayout()
    $bmp = [System.Windows.Media.Imaging.RenderTargetBitmap]::new([int]($Root.ActualWidth * 2), [int]($Root.ActualHeight * 2), 192, 192, [System.Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($Root); $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder; $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bmp))
    $fs = [IO.File]::Create($Preview); $enc.Save($fs); $fs.Close(); return
  }
  $w = New-Object System.Windows.Window
  $w.Title = $Bar; $w.Content = $Root; $w.SizeToContent = 'WidthAndHeight'; $w.ResizeMode = 'NoResize'
  $w.WindowStartupLocation = 'CenterScreen'; $w.Topmost = $true; $w.ShowInTaskbar = $true
  for ($i = 1; $i -le 3; $i++) { $btn = $Root.FindName("B$i"); if ($btn) { $btn.Tag = $i; $btn.Add_Click({
    $n = [int]$this.Tag
    if ($script:PswUiStay -and $script:PswUiStay.ContainsKey($n)) { try { & $script:PswUiStay[$n] } catch { } }   # opens something, the window stays
    else { $script:PswUiResult = $n; [System.Windows.Window]::GetWindow($this).Close() } }) } }
  Set-PswDarkTitle $w; [void]$w.ShowDialog()
}
function Show-PswDialog([string]$Bar, [string]$Head, [string]$Text, [string]$One, [string]$Two = '', [string]$Three = '', [string]$Preview = '', [hashtable]$Stay = @{}) {
  $btns = @($One) + @(@($Two, $Three) | Where-Object { $_ })
  $script:PswUiResult = $btns.Count; $script:PswUiStay = $Stay
  $r = New-PswUiRoot $Head $Text $btns
  Show-PswUiRoot $r $Bar $Preview
  $script:PswUiStay = @{}
  $script:PswUiResult
}
function Show-PswChoice([string]$Bar, [string]$Head, [string]$Text, [string[]]$Items, [int]$Selected, [string]$One, [string]$Two, [string]$Preview = '') {
  $e = { param($t) [Security.SecurityElement]::Escape($t) }
  $rows = ''; for ($i = 0; $i -lt $Items.Count; $i++) { $rows += "<RadioButton x:Name=""R$i"" GroupName=""C"" Content=""$(& $e $Items[$i])""$(if ($i -eq $Selected) { ' IsChecked="True"' })/>" }
  $mid = "<Border Background=""#242424"" BorderBrush=""#333333"" BorderThickness=""1"" CornerRadius=""10"" Padding=""18,10"" Margin=""0,18,0,0""><StackPanel>$rows</StackPanel></Border>"
  $script:PswUiResult = 2; $script:PswUiStay = @{}
  $r = New-PswUiRoot $Head $Text @($One, $Two) $mid
  Show-PswUiRoot $r $Bar $Preview
  if ($script:PswUiResult -ne 1) { return -1 }
  for ($i = 0; $i -lt $Items.Count; $i++) { if ($r.FindName("R$i").IsChecked) { return $i } }
  -1
}
# <<< psw-ui

# ---------------------------------------------------------------- window
$xaml = @"
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Width="1100" Height="720" Background="#1B1B1B" TextOptions.TextFormattingMode="Display" UseLayoutRounding="True" SnapsToDevicePixels="True">
  <Grid.Resources>
    <Style x:Key="Btn" TargetType="Button">
      <Setter Property="FontFamily" Value="Segoe UI Semibold"/><Setter Property="FontSize" Value="13"/><Setter Property="Height" Value="34"/>
      <Setter Property="Cursor" Value="Hand"/><Setter Property="Padding" Value="16,0"/><Setter Property="Foreground" Value="#F2F2F0"/>
      <Setter Property="Background" Value="#2A2A2A"/><Setter Property="BorderBrush" Value="#3E3E3E"/><Setter Property="BorderThickness" Value="1"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
        <Border x:Name="B" CornerRadius="5" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="{TemplateBinding BorderThickness}" Padding="{TemplateBinding Padding}">
          <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
        <ControlTemplate.Triggers>
          <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="B" Property="Opacity" Value="0.88"/></Trigger>
          <Trigger Property="IsPressed" Value="True"><Setter TargetName="B" Property="Opacity" Value="0.75"/></Trigger>
          <Trigger Property="IsEnabled" Value="False"><Setter TargetName="B" Property="Opacity" Value="0.38"/></Trigger>
        </ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style x:Key="Primary" TargetType="Button" BasedOn="{StaticResource Btn}"><Setter Property="Background" Value="$Blue"/><Setter Property="BorderBrush" Value="$Blue"/><Setter Property="Foreground" Value="#FFFFFF"/></Style>
    <Style x:Key="Nav" TargetType="RadioButton">
      <Setter Property="Foreground" Value="#A9A9A4"/><Setter Property="FontFamily" Value="Segoe UI"/><Setter Property="FontSize" Value="14"/><Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="RadioButton">
        <Border x:Name="B" Background="Transparent" Padding="0,9" Margin="0,1">
          <Grid><Grid.ColumnDefinitions><ColumnDefinition Width="3"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
            <Border x:Name="Bar" Background="$Blue" Visibility="Hidden"/>
            <ContentPresenter Grid.Column="1" Margin="15,0,0,0"/></Grid></Border>
        <ControlTemplate.Triggers>
          <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="B" Property="Background" Value="#1C1C1C"/></Trigger>
          <Trigger Property="IsChecked" Value="True"><Setter TargetName="B" Property="Background" Value="#202020"/><Setter TargetName="Bar" Property="Visibility" Value="Visible"/><Setter Property="Foreground" Value="#FFFFFF"/><Setter Property="FontFamily" Value="Segoe UI Semibold"/></Trigger>
        </ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style x:Key="Switch" TargetType="CheckBox">
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="CheckBox">
        <Grid Background="Transparent" Width="44" Height="22">
          <Border x:Name="Track" CornerRadius="11" Background="#2A2A2A" BorderBrush="#5A5A5A" BorderThickness="1"/>
          <Ellipse x:Name="Knob" Width="12" Height="12" Fill="#C9C9C4" HorizontalAlignment="Left" Margin="5,0,0,0"/>
        </Grid>
        <ControlTemplate.Triggers>
          <Trigger Property="IsChecked" Value="True"><Setter TargetName="Track" Property="Background" Value="$Blue"/><Setter TargetName="Track" Property="BorderBrush" Value="$Blue"/>
            <Setter TargetName="Knob" Property="HorizontalAlignment" Value="Right"/><Setter TargetName="Knob" Property="Margin" Value="0,0,5,0"/><Setter TargetName="Knob" Property="Fill" Value="#FFFFFF"/></Trigger>
        </ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style x:Key="Card" TargetType="Border"><Setter Property="Background" Value="#1F1F1F"/><Setter Property="BorderBrush" Value="#303030"/><Setter Property="BorderThickness" Value="1"/><Setter Property="CornerRadius" Value="4"/></Style>
    <Style x:Key="Cap" TargetType="TextBlock"><Setter Property="FontFamily" Value="Segoe UI Semibold"/><Setter Property="FontSize" Value="11.5"/><Setter Property="Foreground" Value="#8A8A85"/></Style>
    <Style x:Key="Head" TargetType="TextBlock"><Setter Property="FontFamily" Value="Segoe UI Semibold"/><Setter Property="FontSize" Value="24"/><Setter Property="Foreground" Value="#F2F2F0"/></Style>
  </Grid.Resources>
  <Grid.ColumnDefinitions><ColumnDefinition Width="210"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
  <!-- sidebar -->
  <Border Background="#151515" BorderBrush="#2A2A2A" BorderThickness="0,0,1,0">
    <DockPanel Margin="0,22,0,18">
      <StackPanel DockPanel.Dock="Top" Orientation="Horizontal" Margin="18,0,0,26">
        <Grid Width="30" Height="30"><Ellipse Fill="$Blue"/><Path Data="M10,9 L15.5,15 L10,21 M16,9 L21.5,15 L16,21" Stroke="#151515" StrokeThickness="3.2" StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round"/></Grid>
        <TextBlock Margin="10,0,0,0" VerticalAlignment="Center" FontFamily="Segoe UI" FontWeight="Bold" FontSize="20"><Run Text="PSW" Foreground="#F2F2F0"/><Run Text="loader" Foreground="$Blue"/></TextBlock>
      </StackPanel>
      <StackPanel DockPanel.Dock="Bottom" Margin="18,0,12,0">
        <StackPanel Orientation="Horizontal"><Rectangle x:Name="SideDot" Width="8" Height="8" Fill="#7A7A75" VerticalAlignment="Center"/>
          <TextBlock x:Name="SideState" Margin="8,0,0,0" FontSize="12.5" Foreground="#C9C9C4"/></StackPanel>
        <TextBlock x:Name="SideVersion" FontSize="11.5" Foreground="#7A7A75" Margin="0,6,0,0" TextWrapping="Wrap"/>
      </StackPanel>
      <StackPanel>
        <RadioButton x:Name="NavOverview" Style="{StaticResource Nav}" GroupName="N" Content="Overview" IsChecked="True"/>
        <RadioButton x:Name="NavPower" Style="{StaticResource Nav}" GroupName="N" Content="Power &amp; Fan"/>
        <RadioButton x:Name="NavHealth" Style="{StaticResource Nav}" GroupName="N" Content="Health"/>
        <RadioButton x:Name="NavTools" Style="{StaticResource Nav}" GroupName="N" Content="Tools"/>
        <RadioButton x:Name="NavUpdates" Style="{StaticResource Nav}" GroupName="N" Content="Updates"/>
      </StackPanel>
    </DockPanel>
  </Border>
  <!-- pages -->
  <Grid Grid.Column="1" Margin="28,36,28,20">
    <!-- overview -->
    <Grid x:Name="PageOverview">
      <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="*"/></Grid.RowDefinitions>
      <DockPanel>
        <StackPanel x:Name="StatusBanner" DockPanel.Dock="Right" Orientation="Horizontal" VerticalAlignment="Center" Cursor="Hand" Background="Transparent">
          <Rectangle x:Name="StatusDot" Width="9" Height="9" Fill="#7A7A75" VerticalAlignment="Center"/>
          <TextBlock x:Name="StatusHead" Margin="8,0,0,0" FontSize="13.5" Foreground="#F2F2F0" VerticalAlignment="Center"/>
          <TextBlock x:Name="StatusText" Margin="8,0,0,0" FontSize="13.5" Foreground="#9A9A95" VerticalAlignment="Center"/>
          <TextBlock Margin="12,0,0,0" FontSize="13.5" Foreground="$Blue" VerticalAlignment="Center" Text="Health"/>
        </StackPanel>
        <TextBlock Text="Overview" Style="{StaticResource Head}"/></DockPanel>
      <Border Grid.Row="1" Style="{StaticResource Card}" Margin="0,14,0,12"><UniformGrid x:Name="Facts" Rows="1"/></Border>
      <UniformGrid x:Name="Tiles" Grid.Row="2" Columns="2" Margin="0,0,-12,0"/>
      <Grid Grid.Row="3"><Grid.ColumnDefinitions><ColumnDefinition Width="1.25*"/><ColumnDefinition Width="12"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
        <Border Style="{StaticResource Card}" Padding="16,10,16,6"><DockPanel><TextBlock DockPanel.Dock="Top" Style="{StaticResource Cap}" Text="HARDWARE" Margin="0,0,0,4"/><StackPanel x:Name="Specs"/></DockPanel></Border>
        <Border Grid.Column="2" Style="{StaticResource Card}" Padding="16,10,16,6"><DockPanel>
          <DockPanel DockPanel.Dock="Top" Margin="0,0,0,4"><TextBlock x:Name="DiskLive" DockPanel.Dock="Right" FontSize="11.5" Foreground="#8A8A85"/><TextBlock Style="{StaticResource Cap}" Text="STORAGE"/></DockPanel>
          <StackPanel x:Name="Disks"/></DockPanel></Border>
      </Grid>
    </Grid>
    <!-- power & fan -->
    <Grid x:Name="PagePower" Visibility="Collapsed">
      <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="*"/></Grid.RowDefinitions>
      <DockPanel Margin="0,0,0,14">
        <Border DockPanel.Dock="Right" Background="#1F1F1F" BorderBrush="#303030" BorderThickness="1" CornerRadius="14" Padding="12,5" VerticalAlignment="Center">
          <StackPanel Orientation="Horizontal">
            <Ellipse Width="7" Height="7" Fill="#4EC97A" VerticalAlignment="Center" Margin="0,0,7,0"/>
            <TextBlock x:Name="FanLiveReadout" FontFamily="Segoe UI Semibold" FontSize="12.5" Foreground="#E6E6E2" VerticalAlignment="Center" Text="SoC -  &#x00B7;  Fan -"/>
          </StackPanel>
        </Border>
        <TextBlock Text="Power &amp; Fan" Style="{StaticResource Head}"/>
      </DockPanel>
      <Border Grid.Row="1" Style="{StaticResource Card}" Padding="16,12" Margin="0,0,0,12">
        <StackPanel>
          <TextBlock Style="{StaticResource Cap}" Text="POWER PRESET" Margin="0,0,0,10"/>
          <UniformGrid Rows="1" Margin="0,0,-10,0">
            <Border x:Name="PwrEff" Background="#242424" BorderBrush="#343434" BorderThickness="1" CornerRadius="6" Padding="14,10" Margin="0,0,10,0" Cursor="Hand">
              <StackPanel>
                <TextBlock Text="Efficiency" FontFamily="Segoe UI Semibold" FontSize="14" Foreground="#F2F2F0"/>
                <TextBlock Text="Lower clocks &#x00B7; Quietest &amp; coolest" FontSize="11.5" Foreground="#9A9A95" Margin="0,3,0,0" TextWrapping="Wrap"/>
              </StackPanel>
            </Border>
            <Border x:Name="PwrStd" Background="#1D2B3D" BorderBrush="#3A86E0" BorderThickness="1.5" CornerRadius="6" Padding="14,10" Margin="0,0,10,0" Cursor="Hand">
              <StackPanel>
                <TextBlock Text="Standard" FontFamily="Segoe UI Semibold" FontSize="14" Foreground="#FFFFFF"/>
                <TextBlock Text="Balanced stock mode &#x00B7; Default" FontSize="11.5" Foreground="#C8DCF5" Margin="0,3,0,0" TextWrapping="Wrap"/>
              </StackPanel>
            </Border>
            <Border x:Name="PwrOc" Background="#242424" BorderBrush="#343434" BorderThickness="1" CornerRadius="6" Padding="14,10" Margin="0,0,10,0" Cursor="Hand">
              <StackPanel>
                <TextBlock Text="Power" FontFamily="Segoe UI Semibold" FontSize="14" Foreground="#F2F2F0"/>
                <TextBlock Text="Full boost clocks &#x00B7; Higher thermal limit" FontSize="11.5" Foreground="#9A9A95" Margin="0,3,0,0" TextWrapping="Wrap"/>
              </StackPanel>
            </Border>
          </UniformGrid>
        </StackPanel>
      </Border>
      <Border Grid.Row="2" Style="{StaticResource Card}" Padding="16,12">
        <Grid>
          <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
          <DockPanel Margin="0,0,0,10">
            <StackPanel DockPanel.Dock="Right" Orientation="Horizontal">
              <Button x:Name="FanSilent" Style="{StaticResource Btn}" Content="Silent" Height="28" Padding="12,0" FontSize="12" Margin="0,0,6,0"/>
              <Button x:Name="FanBalanced" Style="{StaticResource Primary}" Content="Balanced" Height="28" Padding="12,0" FontSize="12" Margin="0,0,6,0"/>
              <Button x:Name="FanPerf" Style="{StaticResource Btn}" Content="Performance" Height="28" Padding="12,0" FontSize="12" Margin="0,0,6,0"/>
              <Button x:Name="FanCustom" Style="{StaticResource Btn}" Content="Custom" Height="28" Padding="12,0" FontSize="12"/>
            </StackPanel>
            <StackPanel VerticalAlignment="Center">
              <TextBlock Style="{StaticResource Cap}" Text="FAN CURVE"/>
              <TextBlock x:Name="FanHint" FontSize="11.5" Foreground="#8A8A85" Margin="0,2,0,0" Text="Drag points to adjust &#x00B7; Click graph to add (max 8) &#x00B7; Right-click point to remove"/>
            </StackPanel>
          </DockPanel>
          <Border Grid.Row="1" Background="#161616" BorderBrush="#2C2C2C" BorderThickness="1" CornerRadius="6" Padding="4" Margin="0,0,0,12">
            <Canvas x:Name="FanCanvas" Background="Transparent" ClipToBounds="True" Cursor="Cross"/>
          </Border>
          <DockPanel Grid.Row="2">
            <StackPanel DockPanel.Dock="Right" Orientation="Horizontal">
              <Button x:Name="FanReset" Style="{StaticResource Btn}" Content="Reset" Margin="0,0,8,0"/>
              <Button x:Name="FanApply" Style="{StaticResource Primary}" Content="Apply" MinWidth="92"/>
            </StackPanel>
            <TextBlock x:Name="PwrStatus" FontSize="12.5" Foreground="#9A9A95" VerticalAlignment="Center" Text="Changes apply live within 1 second &#x00B7; Emergency 100 % at 90 &#x00B0;C."/>
          </DockPanel>
        </Grid>
      </Border>
    </Grid>
    <!-- health -->
    <Grid x:Name="PageHealth" Visibility="Collapsed">
      <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
      <DockPanel>
        <StackPanel DockPanel.Dock="Right" Orientation="Horizontal" VerticalAlignment="Center">
          <Button x:Name="FixNow" Style="{StaticResource Btn}" IsEnabled="False" Margin="0,0,10,0">
            <StackPanel Orientation="Horizontal"><TextBlock Text="Troubleshoot" VerticalAlignment="Center"/>
              <Border Background="#3B2A12" CornerRadius="4" Padding="5,1" Margin="8,0,0,0" VerticalAlignment="Center"><TextBlock Text="EXPERIMENTAL" FontSize="9.5" Foreground="#E8A33D" FontFamily="Segoe UI Semibold"/></Border></StackPanel></Button>
          <Button x:Name="CheckNow" Style="{StaticResource Primary}" Content="Check now"/>
        </StackPanel>
        <TextBlock Text="Health" Style="{StaticResource Head}"/></DockPanel>
      <TextBlock x:Name="HealthSub" Grid.Row="1" FontSize="13" Foreground="#9A9A95" Margin="0,6,0,14" TextWrapping="Wrap"/>
      <Border Grid.Row="2" Style="{StaticResource Card}" Padding="16,4">
        <ScrollViewer VerticalScrollBarVisibility="Hidden"><StackPanel x:Name="HealthList"/></ScrollViewer></Border>
      <TextBlock x:Name="FixNote" Grid.Row="3" FontSize="12" Foreground="#8A8A85" Margin="2,10,0,0" TextWrapping="Wrap"
                 Text="Troubleshooting is experimental: it runs a fresh check first and is only offered when the check finds something it can fix."/>
    </Grid>
    <!-- tools -->
    <StackPanel x:Name="PageTools" Visibility="Collapsed">
      <TextBlock Text="Tools" Style="{StaticResource Head}" Margin="0,0,0,14"/>
      <Border Style="{StaticResource Card}"><StackPanel x:Name="ToolsHelp"/></Border>
      <Border Style="{StaticResource Card}" Padding="22,14" Margin="0,12,0,0">
        <DockPanel>
          <CheckBox x:Name="ToolAutostart" DockPanel.Dock="Right" Style="{StaticResource Switch}" VerticalAlignment="Center" Margin="16,0,0,0"/>
          <StackPanel><TextBlock Text="Start with Windows" FontFamily="Segoe UI Semibold" FontSize="14" Foreground="#F2F2F0"/>
            <TextBlock FontSize="12.5" Foreground="#9A9A95" TextWrapping="Wrap" Text="Open the PSWloader Dashboard when you sign in."/></StackPanel>
        </DockPanel>
      </Border>
    </StackPanel>
    <!-- updates -->
    <StackPanel x:Name="PageUpdates" Visibility="Collapsed">
      <TextBlock Text="Updates" Style="{StaticResource Head}" Margin="0,0,0,14"/>
      <Border Style="{StaticResource Card}" Padding="22,18">
        <StackPanel>
          <Grid><Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
            <StackPanel><TextBlock Style="{StaticResource Cap}" Text="INSTALLED"/><TextBlock x:Name="UpdInstalled" FontFamily="Segoe UI Semibold" FontSize="20" Foreground="#F2F2F0" Margin="0,4,0,0"/><TextBlock x:Name="UpdFirmware" FontSize="12.5" Foreground="#9A9A95" Margin="0,2,0,0"/></StackPanel>
            <StackPanel Grid.Column="1"><TextBlock x:Name="UpdLatestCap" Style="{StaticResource Cap}" Text="LATEST RELEASE"/><TextBlock x:Name="UpdLatest" FontFamily="Segoe UI Semibold" FontSize="20" Foreground="#F2F2F0" Margin="0,4,0,0" Text="-"/><TextBlock x:Name="UpdLatestDate" FontSize="12.5" Foreground="#9A9A95" Margin="0,2,0,0"/></StackPanel>
          </Grid>
          <TextBlock x:Name="UpdState" FontSize="13.5" Foreground="#DADAD5" Margin="0,18,0,16" TextWrapping="Wrap" Text="Look for a new PSWloader release on GitHub."/>
          <StackPanel Orientation="Horizontal">
            <Button x:Name="UpdCheck" Style="{StaticResource Primary}" Content="Check for updates"/>
            <Button x:Name="UpdInstall" Style="{StaticResource Btn}" Content="Download and install" Margin="10,0,0,0" IsEnabled="False"/>
            <Button x:Name="UpdNotes" Style="{StaticResource Btn}" Content="Release notes" Margin="10,0,0,0"/>
          </StackPanel>
          <TextBlock FontSize="12" Foreground="#7A7A75" Margin="0,14,0,0" TextWrapping="Wrap" Text="Installing an update needs administrator rights (drivers are installed)."/>
        </StackPanel>
      </Border>
      <Border Style="{StaticResource Card}" Padding="22,14" Margin="0,12,0,0">
        <DockPanel>
          <CheckBox x:Name="UpdAuto" DockPanel.Dock="Right" Style="{StaticResource Switch}" VerticalAlignment="Center" Margin="16,0,0,0"/>
          <StackPanel><TextBlock Text="Automatic update check" FontFamily="Segoe UI Semibold" FontSize="14" Foreground="#F2F2F0"/>
            <TextBlock FontSize="12.5" Foreground="#9A9A95" TextWrapping="Wrap" Text="Look for a new PSWloader release at every sign-in and offer it. When this is off, look for updates here."/></StackPanel>
        </DockPanel>
      </Border>
      <Border Style="{StaticResource Card}" Padding="22,14" Margin="0,12,0,0">
        <DockPanel>
          <CheckBox x:Name="UpdBeta" DockPanel.Dock="Right" Style="{StaticResource Switch}" VerticalAlignment="Center" Margin="16,0,0,0"/>
          <StackPanel><TextBlock Text="Beta versions" FontFamily="Segoe UI Semibold" FontSize="14" Foreground="#F2F2F0"/>
            <TextBlock FontSize="12.5" Foreground="#9A9A95" TextWrapping="Wrap" Text="Also offer beta releases. Beta versions are experimental: drivers can stop working or your installation can be damaged. Use them at your own risk. An update is only offered when it is newer than the installed version."/></StackPanel>
        </DockPanel>
      </Border>
    </StackPanel>
  </Grid>
</Grid>
"@
$root = [Windows.Markup.XamlReader]::Parse($xaml)
$u = @{}; foreach ($n in 'SideDot','SideState','SideVersion','NavOverview','NavPower','NavHealth','NavTools','NavUpdates','PageOverview','PagePower','PageHealth','PageTools','PageUpdates',
                      'Facts','StatusBanner','StatusDot','StatusHead','StatusText','Tiles','Specs','Disks','DiskLive',
                      'FanLiveReadout','FanHint','PwrEff','PwrStd','PwrOc','FanSilent','FanBalanced','FanPerf','FanCustom','FanCanvas','FanReset','FanApply','PwrStatus',
                      'FixNow','CheckNow','HealthSub','HealthList','FixNote','ToolsHelp',
                      'UpdInstalled','UpdFirmware','UpdLatest','UpdLatestDate','UpdLatestCap','UpdState','UpdCheck','UpdInstall','UpdNotes','UpdBeta','UpdAuto','ToolAutostart') { $u[$n] = $root.FindName($n) }
$bc = New-Object System.Windows.Media.BrushConverter
function Br([string]$c) { $bc.ConvertFromString($c) }
function T([string]$Text, [double]$Size = 13, [string]$Color = '#B4B4AE', [string]$Font = 'Segoe UI') {
  $t = New-Object System.Windows.Controls.TextBlock; $t.Text = $Text; $t.FontSize = $Size; $t.Foreground = (Br $Color); $t.FontFamily = $Font; $t.TextWrapping = 'Wrap'; $t }
function Size([double]$Bytes) { if ($Bytes -ge 1TB) { '{0:0.0} TB' -f ($Bytes / 1TB) } elseif ($Bytes -ge 1GB) { '{0:0} GB' -f ($Bytes / 1GB) } else { '{0:0} MB' -f ($Bytes / 1MB) } }
function Rate([double]$B) { if ($B -ge 1MB) { '{0:0.0} MB/s' -f ($B / 1MB) } elseif ($B -ge 1KB) { '{0:0} KB/s' -f ($B / 1KB) } else { '{0:0} B/s' -f $B } }

# ---------------------------------------------------------------- data (everything read on this PC - nothing fixed in the script)
# v1.2 (user 26.11.2026: the window took 5-10 s to appear): the data is read by script text that runs in a background
# runspace after the window is shown, and with direct CIM classes instead of the Storage and NetAdapter modules
# (measured on the console: Get-PhysicalDisk 2.1 s, Win32_Processor 1.0 s, Get-NetAdapter 0.8 s, Get-Partition/Volume
# 0.45 s -> MSFT_* classes 0.2 s). The number of cores comes from Win32_Processor in a second runspace.
$InfoScript = @'
param($Data, $ChecksFile)
$i = @{}
$vj = $null; try { $vj = Get-Content "$Data\version.json" -Raw | ConvertFrom-Json } catch { }
$i.Version = if ($vj -and $vj.version) { $vj.version } else { 'unknown' }
$i.Firmware = [string](Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS' -ErrorAction SilentlyContinue).BIOSVersion
if ($i.Firmware -notmatch 'r\d+\.\d+') { $i.Firmware = [string](Get-CimInstance Win32_BIOS -ErrorAction SilentlyContinue).SMBIOSBIOSVersion }   # v1.3: the registry value was empty on a Windows (HW 26.11.)
$ch = $null; try { $ch = Get-Content $ChecksFile -Raw | ConvertFrom-Json } catch { }
$i.Checks = $ch; $i.Console = if ($ch -and $ch.console) { $ch.console } else { $null }
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$os = Get-CimInstance Win32_OperatingSystem
$i.Windows = ($os.Caption -replace 'Microsoft ', ''); $i.WinVer = "$($cv.DisplayVersion)"; $i.WinBuild = "$($cv.CurrentBuild).$($cv.UBR)"
$i.Boot = $os.LastBootUpTime
$i.Cpu = ([string](Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0' -ErrorAction SilentlyContinue).ProcessorNameString).Trim()
$i.Threads = [Environment]::ProcessorCount; $i.Cores = 0
$vc = Get-CimInstance Win32_VideoController | Where-Object { $_.PNPDeviceID -match 'VEN_1002' } | Select-Object -First 1
if (-not $vc) { $vc = Get-CimInstance Win32_VideoController | Select-Object -First 1 }
$i.Gpu = $vc.Name; $i.GpuDriver = $vc.DriverVersion; $i.Vram = $null
foreach ($k in Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -ErrorAction SilentlyContinue) {
  $q = (Get-ItemProperty $k.PSPath -Name 'HardwareInformation.qwMemorySize' -ErrorAction SilentlyContinue).'HardwareInformation.qwMemorySize'; if ($q) { $i.Vram = [uint64]$q; break } }
$i.Display = if ($vc.CurrentHorizontalResolution) { "$($vc.CurrentHorizontalResolution) x $($vc.CurrentVerticalResolution), $($vc.CurrentRefreshRate) Hz" } else { 'no display' }
$mon = Get-CimInstance -Namespace root\wmi WmiMonitorID -ErrorAction SilentlyContinue | Select-Object -First 1
$i.Monitor = if ($mon -and $mon.UserFriendlyName) { -join ($mon.UserFriendlyName | Where-Object { $_ } | ForEach-Object { [char]$_ }) } else { '' }
$i.RamTotal = [double](Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
# storage: only drives with a medium (empty card-reader slots report no size)
$ns = 'root/Microsoft/Windows/Storage'
$parts = @(Get-CimInstance -Namespace $ns -ClassName MSFT_Partition -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -and [int]$_.DriveLetter -ne 0 })
$vols = @(Get-CimInstance -Namespace $ns -ClassName MSFT_Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -and [int]$_.DriveLetter -ne 0 })
$i.Disks = @(Get-CimInstance -Namespace $ns -ClassName MSFT_PhysicalDisk -ErrorAction SilentlyContinue | Where-Object { $_.Size -gt 0 } | Sort-Object { [int]$_.DeviceId } | ForEach-Object {
  $dn = [int]$_.DeviceId; $letters = @($parts | Where-Object { [int]$_.DiskNumber -eq $dn } | ForEach-Object { $_.DriveLetter })
  [pscustomobject]@{ Name = $_.FriendlyName; Size = [double]$_.Size; Bus = $(switch ([int]$_.BusType) { 17 { 'NVMe' } 7 { 'USB' } 11 { 'SATA' } default { '' } })
                     Vols = @($vols | Where-Object { $letters -contains $_.DriveLetter } | ForEach-Object { [pscustomobject]@{ DriveLetter = [string]$_.DriveLetter; Size = [double]$_.Size; SizeRemaining = [double]$_.SizeRemaining } }) } })
$na = @(Get-CimInstance -Namespace root/StandardCimv2 -ClassName MSFT_NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.MediaConnectState -eq 1 -and -not $_.Virtual })
$i.Net = if ($na.Count) { ($na | ForEach-Object { $sp = [double]$_.Speed; "$($_.Name) - $(if ($sp -ge 1e9) { '{0:0.#} Gbps' -f ($sp / 1e9) } else { '{0:0} Mbps' -f ($sp / 1e6) })" }) -join ', ' } else { 'not connected' }
$i
'@
function Get-Info { & ([scriptblock]::Create($InfoScript)) $Data $ChecksFile }

# v1.4: lightweight background refresh for dynamic hardware state (monitor/resolution, network adapters/speed,
# storage/USB drives & free space, GPU driver/VRAM, and external checks.json updates) - never blocks the UI thread.
$DynScript = @'
param($ChecksFile, $LastChecksTicks)
$d = @{}
$vc = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -match 'VEN_1002' } | Select-Object -First 1
if (-not $vc) { $vc = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object -First 1 }
if ($vc) {
  $d.Gpu = $vc.Name; $d.GpuDriver = $vc.DriverVersion
  $d.Display = if ($vc.CurrentHorizontalResolution) { "$($vc.CurrentHorizontalResolution) x $($vc.CurrentVerticalResolution), $($vc.CurrentRefreshRate) Hz" } else { 'no display' }
}
$mon = Get-CimInstance -Namespace root\wmi WmiMonitorID -ErrorAction SilentlyContinue | Select-Object -First 1
$d.Monitor = if ($mon -and $mon.UserFriendlyName) { -join ($mon.UserFriendlyName | Where-Object { $_ } | ForEach-Object { [char]$_ }) } else { '' }
$na = @(Get-CimInstance -Namespace root/StandardCimv2 -ClassName MSFT_NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.MediaConnectState -eq 1 -and -not $_.Virtual })
$d.Net = if ($na.Count) { ($na | ForEach-Object { $sp = [double]$_.Speed; "$($_.Name) - $(if ($sp -ge 1e9) { '{0:0.#} Gbps' -f ($sp / 1e9) } else { '{0:0} Mbps' -f ($sp / 1e6) })" }) -join ', ' } else { 'not connected' }
$ns = 'root/Microsoft/Windows/Storage'
$parts = @(Get-CimInstance -Namespace $ns -ClassName MSFT_Partition -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -and [int]$_.DriveLetter -ne 0 })
$vols = @(Get-CimInstance -Namespace $ns -ClassName MSFT_Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -and [int]$_.DriveLetter -ne 0 })
$d.Disks = @(Get-CimInstance -Namespace $ns -ClassName MSFT_PhysicalDisk -ErrorAction SilentlyContinue | Where-Object { $_.Size -gt 0 } | Sort-Object { [int]$_.DeviceId } | ForEach-Object {
  $dn = [int]$_.DeviceId; $letters = @($parts | Where-Object { [int]$_.DiskNumber -eq $dn } | ForEach-Object { $_.DriveLetter })
  [pscustomobject]@{ Name = $_.FriendlyName; Size = [double]$_.Size; Bus = $(switch ([int]$_.BusType) { 17 { 'NVMe' } 7 { 'USB' } 11 { 'SATA' } default { '' } })
                     Vols = @($vols | Where-Object { $letters -contains $_.DriveLetter } | ForEach-Object { [pscustomobject]@{ DriveLetter = [string]$_.DriveLetter; Size = [double]$_.Size; SizeRemaining = [double]$_.SizeRemaining } }) } })
$cf = Get-Item -LiteralPath $ChecksFile -ErrorAction SilentlyContinue
if ($cf -and $cf.LastWriteTimeUtc.Ticks -gt [int64]$LastChecksTicks) {
  try { $d.Checks = Get-Content $ChecksFile -Raw | ConvertFrom-Json; $d.ChecksTicks = $cf.LastWriteTimeUtc.Ticks } catch { }
}
$d
'@

# live values: Windows performance counters + the PS5 thermal log (C:\ps5-work\thermal-manager.jsonl, written by the
# ThermalManager every few seconds: SoC temperature, fan duty, GPU clock and load). History: 60 samples, one per second.
$script:Hist = @{ cpu = New-Object System.Collections.Generic.List[double]; gpu = New-Object System.Collections.Generic.List[double]
                  ram = New-Object System.Collections.Generic.List[double]; temp = New-Object System.Collections.Generic.List[double] }
$script:Pc = $null
$CounterScript = @'
  $c = @{}
  try { $c.cpu = New-Object Diagnostics.PerformanceCounter('Processor Information', '% Processor Utility', '_Total'); [void]$c.cpu.NextValue() }
  catch { try { $c.cpu = New-Object Diagnostics.PerformanceCounter('Processor', '% Processor Time', '_Total'); [void]$c.cpu.NextValue() } catch { $c.cpu = $null } }
  try { $c.mem = New-Object Diagnostics.PerformanceCounter('Memory', 'Available Bytes'); [void]$c.mem.NextValue() } catch { $c.mem = $null }
  try { $c.disk = New-Object Diagnostics.PerformanceCounter('PhysicalDisk', '% Idle Time', '_Total'); [void]$c.disk.NextValue() } catch { $c.disk = $null }
  $c.rx = @(); $c.tx = @()
  try { foreach ($n in (New-Object Diagnostics.PerformanceCounterCategory('Network Interface')).GetInstanceNames()) {
          $c.rx += New-Object Diagnostics.PerformanceCounter('Network Interface', 'Bytes Received/sec', $n); $c.tx += New-Object Diagnostics.PerformanceCounter('Network Interface', 'Bytes Sent/sec', $n) } } catch { }
  $c
'@
function New-Counters { & ([scriptblock]::Create($CounterScript)) }
function Read-Thermal {
  $f = 'C:\ps5-work\thermal-manager.jsonl'; $r = @{}
  try {
    $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek([Math]::Max(0, $fs.Length - 16384), 'Begin')
    $sr = New-Object IO.StreamReader($fs); $lines = $sr.ReadToEnd() -split "`n"; $sr.Close()
    for ($k = $lines.Count - 1; $k -ge 0 -and ($r.Count -lt 2); $k--) {
      $l = $lines[$k]
      if (-not $r.fan -and $l -match '"event":"fan_status"') { try { $r.fan = $l | ConvertFrom-Json } catch { } }
      elseif (-not $r.gfx -and $l -match '"event":"gfx_measured"') { try { $r.gfx = $l | ConvertFrom-Json } catch { } }
    }
  } catch { }
  foreach ($k in @($r.Keys)) { try { if (((Get-Date) - [datetime]$r[$k].at).TotalSeconds -gt 90) { $r.Remove($k) } } catch { } }   # stale = not live
  $r
}
function Push($List, [double]$V) { $List.Add($V); while ($List.Count -gt 60) { $List.RemoveAt(0) } }

# ---------------------------------------------------------------- overview widgets
# graph panel as in the Windows Task Manager: name and current value, plot with a grid over the last 60 seconds, one colour
function New-Tile([string]$Key, [string]$Caption) {
  $b = New-Object System.Windows.Controls.Border; $b.Style = $root.Resources['Card']; $b.Padding = '14,10,14,8'; $b.Margin = '0,0,12,12'
  $sp = New-Object System.Windows.Controls.StackPanel
  $h = New-Object System.Windows.Controls.DockPanel
  $val = T '-' 17 '#F2F2F0' 'Segoe UI Semibold'; [System.Windows.Controls.DockPanel]::SetDock($val, 'Right'); [void]$h.Children.Add($val)
  $cap = T $Caption 13.5 '#DADAD5' 'Segoe UI Semibold'; $cap.VerticalAlignment = 'Bottom'; [void]$h.Children.Add($cap); [void]$sp.Children.Add($h)
  $pb = New-Object System.Windows.Controls.Border; $pb.BorderBrush = (Br '#3A3A3A'); $pb.BorderThickness = 1; $pb.Background = (Br '#181818'); $pb.Margin = '0,6,0,4'
  $cv = New-Object System.Windows.Controls.Canvas; $cv.Height = 62; $cv.ClipToBounds = $true; $pb.Child = $cv; [void]$sp.Children.Add($pb)
  $f = New-Object System.Windows.Controls.DockPanel
  $rng = T '' 11 '#7A7A75'; [System.Windows.Controls.DockPanel]::SetDock($rng, 'Right'); [void]$f.Children.Add($rng)
  $sub = T '' 11.5 '#9A9A95'; $sub.TextWrapping = 'NoWrap'; $sub.TextTrimming = 'CharacterEllipsis'; [void]$f.Children.Add($sub); [void]$sp.Children.Add($f)
  $b.Child = $sp; $b.Tag = @{ Key = $Key; Value = $val; Sub = $sub; Range = $rng; Canvas = $cv }; $b
}
function Draw-Spark($Tile, $Values, [double]$Min, [double]$Max, [string]$Unit = '%') {
  $t = $Tile.Tag; $cv = $t.Canvas; $cv.Children.Clear()
  $w = $cv.ActualWidth; if ($w -le 0) { $w = 380 }; $h = $cv.Height
  $t.Range.Text = "60 s  |  $([int]$Min) - $([int]$Max) $Unit"
  foreach ($k in 1..3) { $ln = New-Object System.Windows.Shapes.Line; $ln.X1 = 0; $ln.X2 = $w; $ln.Y1 = [Math]::Round($h * $k / 4); $ln.Y2 = $ln.Y1; $ln.Stroke = (Br '#262626'); $ln.StrokeThickness = 1; [void]$cv.Children.Add($ln) }
  foreach ($k in 1..5) { $ln = New-Object System.Windows.Shapes.Line; $ln.Y1 = 0; $ln.Y2 = $h; $ln.X1 = [Math]::Round($w * $k / 6); $ln.X2 = $ln.X1; $ln.Stroke = (Br '#262626'); $ln.StrokeThickness = 1; [void]$cv.Children.Add($ln) }
  $n = $Values.Count; if ($n -lt 2) { return }
  $pts = New-Object System.Windows.Media.PointCollection; $area = New-Object System.Windows.Media.PointCollection
  $step = $w / 59; $x0 = $w - ($n - 1) * $step
  [void]$area.Add([System.Windows.Point]::new($x0, $h))
  for ($k = 0; $k -lt $n; $k++) {
    $v = [Math]::Max($Min, [Math]::Min($Max, $Values[$k])); $y = $h - 1 - ($h - 2) * ($v - $Min) / [Math]::Max(0.001, $Max - $Min)
    $p = [System.Windows.Point]::new($x0 + $k * $step, $y); [void]$pts.Add($p); [void]$area.Add($p) }
  [void]$area.Add([System.Windows.Point]::new($w, $h))
  $pg = New-Object System.Windows.Shapes.Polygon; $pg.Points = $area; $pg.Fill = (Br '#263A86E0'); [void]$cv.Children.Add($pg)
  $pl = New-Object System.Windows.Shapes.Polyline; $pl.Points = $pts; $pl.Stroke = (Br $Blue); $pl.StrokeThickness = 1.5; $pl.StrokeLineJoin = 'Round'; [void]$cv.Children.Add($pl)
}
function Fact([string]$Label, [string]$Value, [string]$Sub, [bool]$First = $false) {
  $b = New-Object System.Windows.Controls.Border; $b.Padding = '16,10'; if (-not $First) { $b.BorderBrush = (Br '#303030'); $b.BorderThickness = '1,0,0,0' }
  $sp = New-Object System.Windows.Controls.StackPanel
  [void]$sp.Children.Add((T $Label.ToUpper() 11 '#8A8A85' 'Segoe UI Semibold'))
  $v = T $Value 15 '#F2F2F0' 'Segoe UI Semibold'; $v.Margin = '0,3,0,0'; $v.TextWrapping = 'NoWrap'; $v.TextTrimming = 'CharacterEllipsis'; [void]$sp.Children.Add($v)
  $x = T $Sub 11.5 '#8A8A85'; $x.TextWrapping = 'NoWrap'; $x.TextTrimming = 'CharacterEllipsis'; [void]$sp.Children.Add($x)
  $b.Child = $sp; $b.Tag = $v; $b
}
function Spec([string]$Label, [string]$Value, [bool]$Last = $false) {   # one table row: label, value
  $b = New-Object System.Windows.Controls.Border; $b.Padding = '0,6'; if (-not $Last) { $b.BorderBrush = (Br '#2A2A2A'); $b.BorderThickness = '0,0,0,1' }
  $g = New-Object System.Windows.Controls.Grid
  foreach ($wd in '92', '*') { $cd = New-Object System.Windows.Controls.ColumnDefinition; $cd.Width = $wd; $g.ColumnDefinitions.Add($cd) }
  $l = T $Label 12.5 '#8A8A85'
  $m = T $Value 12.5 '#E6E6E2'; $m.TextWrapping = 'NoWrap'; $m.TextTrimming = 'CharacterEllipsis'; [System.Windows.Controls.Grid]::SetColumn($m, 1)
  [void]$g.Children.Add($l); [void]$g.Children.Add($m); $b.Child = $g; $b.Tag = $m; $b
}
function Bar([double]$Frac, [string]$Color) {   # used part as a star column - no size events needed
  $f = [Math]::Max(0.01, [Math]::Min(1, $Frac))
  $g = New-Object System.Windows.Controls.Grid; $g.Height = 4; $g.Margin = '0,5,0,3'
  foreach ($wd in $f, (1 - $f)) { $cd = New-Object System.Windows.Controls.ColumnDefinition; $cd.Width = [System.Windows.GridLength]::new([Math]::Max(0.0001, $wd), 'Star'); $g.ColumnDefinitions.Add($cd) }
  $bg = New-Object System.Windows.Controls.Border; $bg.Background = (Br '#303030'); [System.Windows.Controls.Grid]::SetColumnSpan($bg, 2); [void]$g.Children.Add($bg)
  $fg = New-Object System.Windows.Controls.Border; $fg.Background = (Br $Color); [void]$g.Children.Add($fg); $g
}

$script:T = @{ cpu = (New-Tile 'cpu' 'Processor'); gpu = (New-Tile 'gpu' 'Graphics'); ram = (New-Tile 'ram' 'Memory'); temp = (New-Tile 'temp' 'Temperature') }
foreach ($k in 'cpu', 'gpu', 'ram', 'temp') { [void]$u.Tiles.Children.Add($script:T[$k]) }

function Show-Page([string]$Name) {
  foreach ($p in 'Overview', 'Power', 'Health', 'Tools', 'Updates') { $u["Page$p"].Visibility = if ($p -eq $Name) { 'Visible' } else { 'Collapsed' } }
  $u["Nav$Name"].IsChecked = $true
  if ($Name -eq 'Power') { Draw-FanCurve }
}
function Get-Uptime($Boot) { $up = (Get-Date) - $Boot; if ($up.TotalDays -ge 1) { '{0} d {1} h' -f [int][Math]::Floor($up.TotalDays), $up.Hours } else { '{0} h {1} min' -f [int][Math]::Floor($up.TotalHours), $up.Minutes } }
function Fill-Disks($DiskList) {
  $u.Disks.Children.Clear(); $shown = @($DiskList | Select-Object -First 3); $dn = 0
  foreach ($d in $shown) {
    $dn++; $row = New-Object System.Windows.Controls.Border; $row.Padding = '0,6'; if ($dn -lt $shown.Count) { $row.BorderBrush = (Br '#2A2A2A'); $row.BorderThickness = '0,0,0,1' }
    $sp = New-Object System.Windows.Controls.StackPanel
    $top = New-Object System.Windows.Controls.DockPanel
    $sz = T (Size $d.Size) 12.5 '#8A8A85'; [System.Windows.Controls.DockPanel]::SetDock($sz, 'Right'); [void]$top.Children.Add($sz)
    $nm = T $d.Name 12.5 '#E6E6E2'; $nm.TextWrapping = 'NoWrap'; $nm.TextTrimming = 'CharacterEllipsis'; [void]$top.Children.Add($nm)
    [void]$sp.Children.Add($top)
    $bus = switch ($d.Bus) { 'NVMe' { 'Internal M.2' } 'USB' { 'USB' } default { $d.Bus } }
    $v = @($d.Vols | Where-Object { $_.Size -gt 0 })
    if ($v.Count) {
      $tot = ($v | Measure-Object -Property Size -Sum).Sum; $used = $tot - ($v | Measure-Object -Property SizeRemaining -Sum).Sum
      [void]$sp.Children.Add((Bar ($used / $tot) $(if ($used / $tot -gt 0.9) { '#E5534B' } else { $Blue })))
      [void]$sp.Children.Add((T ("$bus, " + (($v | ForEach-Object { "$($_.DriveLetter): $(Size $_.SizeRemaining) free" }) -join ', ')) 11.5 '#8A8A85'))
    } else { [void]$sp.Children.Add((T "$bus, no drive letter" 11.5 '#8A8A85')) }
    $row.Child = $sp; [void]$u.Disks.Children.Add($row)
  }
}
function Fill-Overview($i) {
  $con = $i.Console
  $model = if ($con -and $con.model) { $con.model } else { 'PlayStation 5' }
  $board = if ($con -and $con.board -and $con.board -notmatch 'unknown') { "board $($con.board)" } elseif ($con -and $con.boardId) { "board id $($con.boardId)" } else { '' }
  $board = ((@($(if ($con -and $con.systemSoftware) { "System software $($con.systemSoftware)" }), $(if ($con -and $con.chassis) { "Chassis $($con.chassis)" }), $board) | Where-Object { $_ }) -join ', ')
  if ($board) { $board = $board.Substring(0, 1).ToUpper() + $board.Substring(1) }
  $fwRel = if ($i.Firmware -match '(r\d+(\.\d+)+)') { $Matches[1] } else { $i.Firmware }
  $u.Facts.Children.Clear()
  [void]$u.Facts.Children.Add((Fact 'Console' $model $board $true))
  [void]$u.Facts.Children.Add((Fact 'PSWloader' $i.Version "Firmware $fwRel"))
  [void]$u.Facts.Children.Add((Fact 'Windows' "$($i.Windows -replace '^Windows ', '') $($i.WinVer)" "Build $($i.WinBuild)"))
  $script:UpFact = Fact 'Running for' (Get-Uptime $i.Boot) ("since " + $i.Boot.ToString('dd.MM. HH:mm')); [void]$u.Facts.Children.Add($script:UpFact)
  $u.Specs.Children.Clear()
  $script:CpuSpec = Spec 'Processor' "$($i.Cpu), $(if ($i.Cores) { "$($i.Cores) cores, " })$($i.Threads) threads"; [void]$u.Specs.Children.Add($script:CpuSpec)
  $script:GpuSpec = Spec 'Graphics' $i.Gpu; [void]$u.Specs.Children.Add($script:GpuSpec)
  $script:GpuDrvSpec = Spec 'GPU driver' ((@("$($i.GpuDriver)", $(if ($i.Vram) { "$(Size $i.Vram) video memory" })) | Where-Object { $_ }) -join ', '); [void]$u.Specs.Children.Add($script:GpuDrvSpec)
  $script:MemSpec = Spec 'Memory' "$(Size $i.RamTotal) usable by Windows"; [void]$u.Specs.Children.Add($script:MemSpec)
  $script:DispSpec = Spec 'Display' ((@($i.Display, $i.Monitor) | Where-Object { $_ }) -join ', '); [void]$u.Specs.Children.Add($script:DispSpec)
  $script:NetSpec = Spec 'Network' $i.Net $true; [void]$u.Specs.Children.Add($script:NetSpec)
  Fill-Disks $i.Disks
  $u.SideVersion.Text = "PSWloader $($i.Version)`nFirmware $fwRel"
}
function Update-Live([switch]$Sample) {
  $th = Read-Thermal
  if (-not $Sample) { if (-not $script:Pc) { $script:Pc = New-Counters }
    $p = $script:Pc
    $cpu = if ($p.cpu) { [Math]::Min(100, $p.cpu.NextValue()) } else { 0 }
    $avail = if ($p.mem) { $p.mem.NextValue() } else { 0 }
    $ram = if ($script:Info.RamTotal -gt 0 -and $avail -gt 0) { 100 * (1 - $avail / $script:Info.RamTotal) } else { 0 }
    $dsk = if ($p.disk) { 100 - [Math]::Min(100, $p.disk.NextValue()) } else { $null }
    $rx = 0; foreach ($c in $p.rx) { try { $rx += $c.NextValue() } catch { } }; $tx = 0; foreach ($c in $p.tx) { try { $tx += $c.NextValue() } catch { } }
    Push $script:Hist.cpu $cpu; Push $script:Hist.ram $ram
    if ($th.gfx) { Push $script:Hist.gpu ([double]$th.gfx.gfx_busy_permille / 10) }
    $temp = if ($th.fan) { $(if ($th.fan.zone0_mainsoc) { [double]$th.fan.zone0_mainsoc } else { [double]$th.fan.tctl_c }) } else { $null }
    if ($null -ne $temp) { Push $script:Hist.temp $temp }
  } else { $avail = $script:Info.RamTotal * (1 - $script:Hist.ram[$script:Hist.ram.Count - 1] / 100); $dsk = 2; $rx = 14336; $tx = 3072 }
  if ($null -ne $dsk) { $u.DiskLive.Text = "activity $([int]$dsk) %" }
  if ($script:MemSpec -and $script:Info.RamTotal -gt 0 -and $avail -gt 0) {
    $script:MemSpec.Tag.Text = "$(Size ($script:Info.RamTotal - $avail)) / $(Size $script:Info.RamTotal) used ($(Size $avail) free)"
  }
  if ($script:NetSpec) { $script:NetSpec.Tag.Text = "$($script:Info.Net), down $(Rate $rx), up $(Rate $tx)" }
  if ($script:UpFact) { $script:UpFact.Tag.Text = Get-Uptime $script:Info.Boot }
  $t = $script:T
  $t.cpu.Tag.Value.Text = '{0:0} %' -f $script:Hist.cpu[$script:Hist.cpu.Count - 1]; $t.cpu.Tag.Sub.Text = "$(if ($script:Info.Cores) { "$($script:Info.Cores) cores, " })$($script:Info.Threads) threads"
  Draw-Spark $t.cpu $script:Hist.cpu 0 100
  $t.ram.Tag.Value.Text = '{0:0} %' -f $script:Hist.ram[$script:Hist.ram.Count - 1]; $t.ram.Tag.Sub.Text = "$(Size ($script:Info.RamTotal - $avail)) of $(Size $script:Info.RamTotal) in use"
  Draw-Spark $t.ram $script:Hist.ram 0 100
  if ($th.gfx -or $Sample) {
    $mhz = if ($th.gfx) { $th.gfx.gfx_measured_mhz } else { 2219 }
    $t.gpu.Tag.Value.Text = '{0:0} %' -f $script:Hist.gpu[$script:Hist.gpu.Count - 1]; $t.gpu.Tag.Sub.Text = "GPU clock $mhz MHz"; Draw-Spark $t.gpu $script:Hist.gpu 0 100
  } else { $t.gpu.Tag.Value.Text = '-'; $t.gpu.Tag.Sub.Text = 'no values from the console yet'; $t.gpu.Tag.Canvas.Children.Clear() }
  if ($th.fan -or $Sample) {
    $duty = if ($th.fan) { [int]$th.fan.duty_percent } else { 32 }
    $soc = $script:Hist.temp[$script:Hist.temp.Count - 1]
    $t.temp.Tag.Value.Text = "{0:0} $([char]0xB0)C" -f $soc; $t.temp.Tag.Sub.Text = "SoC, fan at $duty %"
    Draw-Spark $t.temp $script:Hist.temp 30 100 "$([char]0xB0)C"
    $script:LiveSoC = $soc; $script:LiveFan = $duty
    $u.FanLiveReadout.Text = "SoC {0:0.0} $([char]0xB0)C  $([char]0xB7)  Fan {1} %" -f $soc, $duty
    if ($u.PagePower.Visibility -eq 'Visible') { Draw-FanCurve }
  } else { $t.temp.Tag.Value.Text = '-'; $t.temp.Tag.Sub.Text = 'no values from the console yet'; $t.temp.Tag.Canvas.Children.Clear() }
}

# ---------------------------------------------------------------- health
# v1.2: callers wrap it in @() - Windows PowerShell 5.1 unrolls a one-item result to a PSCustomObject without .Count (HW 26.11.:
# Troubleshoot stayed grey with exactly one fixable problem)
function Get-Fixable($ch) { @(@($ch.checks) | Where-Object { $_.fix -and $_.state -ge 1 -and $_.fix -ne 'restart' }) }
function Fill-Health($i) {
  $ch = $i.Checks; $u.HealthList.Children.Clear()
  if (-not $ch) {
    $u.StatusDot.Fill = (Br '#7A7A75'); $u.StatusHead.Text = 'Not checked yet'; $u.StatusText.Text = ''
    $u.SideDot.Fill = (Br '#7A7A75'); $u.SideState.Text = 'Not checked yet'
    $u.HealthSub.Text = 'PSWloader checks itself at every start and every 6 hours.'; $u.FixNow.IsEnabled = $false; return }
  $list = @($ch.checks); $np = @($list | Where-Object { $_.state -eq 2 }).Count; $nn = @($list | Where-Object { $_.state -eq 1 }).Count
  $when = try { ([datetime]$ch.time).ToString('dd.MM.yyyy HH:mm') } catch { $ch.time }
  if ($np) { $col = '#E5534B'; $u.StatusHead.Text = "$np problem$(if ($np -gt 1) { 's' })" }
  elseif ($nn) { $col = '#E8A33D'; $u.StatusHead.Text = "$nn note$(if ($nn -gt 1) { 's' })" }
  else { $col = '#7BC67E'; $u.StatusHead.Text = 'No problems' }
  $u.StatusDot.Fill = (Br $col); $u.SideDot.Fill = (Br $col); $u.SideState.Text = $u.StatusHead.Text
  $u.StatusText.Text = "checked $when"
  $u.HealthSub.Text = "Last check: $when, $($list.Count) checks. PSWloader checks itself at every start and every 6 hours."
  $fx = @(Get-Fixable $ch); $u.FixNow.IsEnabled = ($fx.Count -gt 0)
  $sorted = @($list | Sort-Object { - $_.state }); $rn = 0
  foreach ($c in $sorted) {
    $rn++; $row = New-Object System.Windows.Controls.Border; $row.Padding = '0,9'; if ($rn -lt $sorted.Count) { $row.BorderBrush = (Br '#2A2A2A'); $row.BorderThickness = '0,0,0,1' }
    $g = New-Object System.Windows.Controls.Grid
    foreach ($wd in '26', '176', '*') { $cd = New-Object System.Windows.Controls.ColumnDefinition; $cd.Width = $wd; $g.ColumnDefinitions.Add($cd) }
    $ic = T ([string]@([char]0xE73E, [char]0xE7BA, [char]0xE711)[$c.state]) 12.5 @('#7BC67E', '#E8A33D', '#E5534B')[$c.state] 'Segoe MDL2 Assets'; $ic.Margin = '0,2,0,0'
    $nm = T $c.name 13 '#F2F2F0' $(if ($c.state) { 'Segoe UI Semibold' } else { 'Segoe UI' }); [System.Windows.Controls.Grid]::SetColumn($nm, 1)
    $dt = T $c.detail 13 $(if ($c.state) { '#DADAD5' } else { '#9A9A95' }); [System.Windows.Controls.Grid]::SetColumn($dt, 2)
    [void]$g.Children.Add($ic); [void]$g.Children.Add($nm); [void]$g.Children.Add($dt); $row.Child = $g; [void]$u.HealthList.Children.Add($row)
  }
}

# ---------------------------------------------------------------- tools (grouped rows)
function Ask([string]$Head, [string]$Text, [string]$Yes) { (Show-PswDialog -Bar 'PSWloader Dashboard' -Head $Head -Text $Text -One $Yes -Two 'Cancel') -eq 1 }
function Say([string]$Head, [string]$Text) { [void](Show-PswDialog -Bar 'PSWloader Dashboard' -Head $Head -Text $Text -One 'Close') }
function Start-UserTask([string]$Name) { try { Start-ScheduledTask -TaskName $Name -ErrorAction Stop; $true } catch { $false } }
function Row([string]$Title, [string]$Text, [string]$Button, [scriptblock]$Do, [bool]$Last = $false, [bool]$Primary = $false) {
  $b = New-Object System.Windows.Controls.Border; $b.Padding = '18,12'; if (-not $Last) { $b.BorderBrush = (Br '#2A2A2A'); $b.BorderThickness = '0,0,0,1' }
  $g = New-Object System.Windows.Controls.Grid
  foreach ($wd in '*', 'Auto') { $cd = New-Object System.Windows.Controls.ColumnDefinition; $cd.Width = $wd; $g.ColumnDefinitions.Add($cd) }
  $sp = New-Object System.Windows.Controls.StackPanel; $sp.VerticalAlignment = 'Center'
  [void]$sp.Children.Add((T $Title 14 '#F2F2F0' 'Segoe UI Semibold')); [void]$sp.Children.Add((T $Text 12.5 '#9A9A95'))
  $st = T '' 12 '#8FB8EC'; $st.Margin = '0,4,0,0'; $st.Visibility = 'Collapsed'; [void]$sp.Children.Add($st)
  $btn = New-Object System.Windows.Controls.Button; $btn.Content = $Button; $btn.Style = $root.Resources[$(if ($Primary) { 'Primary' } else { 'Btn' })]; $btn.MinWidth = 150
  $btn.VerticalAlignment = 'Center'; $btn.Margin = '16,0,0,0'; $btn.Add_Click($Do); [System.Windows.Controls.Grid]::SetColumn($btn, 1)
  [void]$g.Children.Add($sp); [void]$g.Children.Add($btn); $b.Child = $g
  $b.Tag = @{ Button = $btn; Status = $st }; $b
}
$script:zip = ''
function Fill-Tools {
  $u.ToolsHelp.Children.Clear()
  [void]$u.ToolsHelp.Children.Add((Row 'UEFI Boot Manager' 'Restart straight into it: start another disk, boot order, PSWloader settings.' 'Restart' {
    if (Ask 'UEFI Boot Manager' "Windows restarts now and opens the PSWloader UEFI Boot Manager.`n`nOpen programs are closed - save your work first." 'Restart') {
      $t0 = Get-Date
      if (-not (Start-UserTask 'PSWloader Boot Manager')) { Say 'Restart not available' 'The restart task is missing - run the PSWloader updater once.'; return }
      # v1.3 (HW 26.11.2026: a click restarted nothing and said nothing): the task's result decides - Windows restarts within
      # seconds after a good one; a failed one is reported with its code
      for ($i = 0; $i -lt 24; $i++) {
        Start-Sleep -Milliseconds 500
        $ti = Get-ScheduledTaskInfo -TaskName 'PSWloader Boot Manager' -ErrorAction SilentlyContinue
        if ($ti -and $ti.LastRunTime -ge $t0.AddSeconds(-2) -and (Get-ScheduledTask -TaskName 'PSWloader Boot Manager').State -ne 'Running') {
          if ([int64]$ti.LastTaskResult -ne 0) { Say 'Restart failed' ("Windows refused the restart into the UEFI Boot Manager (code 0x{0:X}). Restart Windows normally and press DEL on the PSWloader screen, or create a support zip." -f [int64]$ti.LastTaskResult) }
          break } } } }))
  $script:SupportRow = Row 'Support zip' 'Check PSWloader and collect the logs into one zip for a GitHub issue. Personal details are removed.' 'Create zip' {
    $t = $script:SupportRow.Tag
    $script:SupportStart = Get-Date
    if (-not (Start-UserTask 'PSWloader Support Zip')) { $t.Status.Text = 'The support task is missing - run the PSWloader updater once.'; $t.Status.Visibility = 'Visible'; return }
    $t.Button.IsEnabled = $false; $t.Status.Text = 'Starting ...'; $t.Status.Visibility = 'Visible'; $script:zip = ''; $script:SupportTimer.Start() } $false $true
  [void]$u.ToolsHelp.Children.Add($script:SupportRow)
  [void]$u.ToolsHelp.Children.Add((Row "What's new" 'The new features of this PSWloader release.' "Show" {
    $s = @("$PSScriptRoot\psw-welcome.ps1", "$Data\psw-welcome.ps1") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($s) { Start-Process powershell.exe -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$s`" -Page WhatsNew -Show" }
    else { Start-Process "https://github.com/$Repo/releases" } }))
  [void]$u.ToolsHelp.Children.Add((Row 'Report a problem' 'Open an issue on GitHub - attach the support zip.' 'Open GitHub' { Start-Process "https://github.com/$Repo/issues/new" }))
  [void]$u.ToolsHelp.Children.Add((Row 'Support the project' 'PSWloader is a personal project without commercial purpose. A coffee keeps it going.' 'Buy me a coffee' { Start-Process 'https://buymeacoffee.com/mrnative' } $true))
}

# progress files are written by the SYSTEM tasks (users only read them): lines of an older run do not count
function Read-Progress([string]$File, [datetime]$Since) {
  try { if ((Get-Item -LiteralPath $File -ErrorAction Stop).LastWriteTime -lt $Since.AddSeconds(-2)) { return @() }
        $fs = [IO.File]::Open($File, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs); $l = @(($sr.ReadToEnd() -split "`r?`n") | Where-Object { $_ }); $sr.Close(); return $l } catch { return @() }
}
# support zip progress (written by the SYSTEM task): "<pct>`t<text>", "ZIP`t<path>" at the end
$script:SupportTimer = New-Object System.Windows.Threading.DispatcherTimer; $script:SupportTimer.Interval = [TimeSpan]::FromMilliseconds(500)
$script:SupportTimer.Add_Tick({
  $t = $script:SupportRow.Tag
  $lines = Read-Progress $SupportProgress $script:SupportStart
  $last = @($lines | Where-Object { $_ -match '^\d+\t' } | Select-Object -Last 1); $z = @($lines | Where-Object { $_ -match '^ZIP\t' } | Select-Object -Last 1)
  if ($last.Count) { $p = $last[0] -split "`t", 2; $t.Status.Text = "$($p[0]) %  $($p[1])" }
  if ($z.Count) {
    $script:SupportTimer.Stop(); $script:zip = ($z[0] -split "`t", 2)[1]
    $t.Status.Text = "Done: $($script:zip)"; $t.Button.IsEnabled = $true
    # "Open folder" and "Open an issue" only open something - the window stays until Close
    [void](Show-PswDialog -Bar 'PSWloader Dashboard' -Head 'Support zip ready' -One 'Open folder' -Two 'Open an issue on GitHub' -Three 'Close' `
           -Text "Attach this file to an issue on GitHub:`n$($script:zip)`n`nPersonal details (user and computer names, Wi-Fi names, MAC addresses, serial numbers) were removed from the files." `
           -Stay @{ 1 = { Start-Process explorer.exe "/select,`"$($script:zip)`"" }; 2 = { Start-Process "https://github.com/$Repo/issues/new" } })
    $script:Info = Get-Info; $script:Info.Cores = $script:Cores; Fill-Health $script:Info
  }
})

# check: starts the SYSTEM task "PSWloader Check" and waits for a newer checks.json; $script:AfterCheck runs once it is there
$script:AfterCheck = $null
$script:CheckTimer = New-Object System.Windows.Threading.DispatcherTimer; $script:CheckTimer.Interval = [TimeSpan]::FromMilliseconds(700)
$script:CheckTimer.Add_Tick({
  $f = Get-Item -LiteralPath $ChecksFile -ErrorAction SilentlyContinue
  $fresh = $f -and $f.LastWriteTime -gt $script:CheckStart; $secs = [int]((Get-Date) - $script:CheckStart).TotalSeconds
  if ($fresh -or $secs -gt 90) {
    $script:CheckTimer.Stop(); $u.CheckNow.IsEnabled = $true; $u.CheckNow.Content = 'Check now'
    $script:Info = Get-Info; $script:Info.Cores = $script:Cores; Fill-Health $script:Info; Fill-Overview $script:Info
    if (-not $fresh) { $u.HealthSub.Text = 'The check did not finish within 90 seconds - the results shown are from the last finished check. Create a support zip if this repeats.' }
    if ($script:AfterCheck) { $a = $script:AfterCheck; $script:AfterCheck = $null; & $a } }
  else { $u.CheckNow.Content = "Checking ... $secs s" }
})
# v1.3 (user 26.11.2026: after "Check now" the old results looked current): while a check runs, every result of the last check is
# marked pending and greyed out, and the status says the check is running; the new results replace them when it is done
function Set-HealthPending {
  $u.StatusDot.Fill = (Br '#7A7A75'); $u.StatusHead.Text = 'Checking ...'; $u.StatusText.Text = 'started ' + (Get-Date).ToString('HH:mm:ss')
  $u.SideDot.Fill = (Br '#7A7A75'); $u.SideState.Text = 'Checking ...'
  $u.HealthSub.Text = 'PSWloader is checking itself. The results below are from the last check and are replaced when it is done.'
  foreach ($row in @($u.HealthList.Children)) {
    $g = $row.Child; if (-not $g -or $g.Children.Count -lt 3) { continue }
    $g.Children[0].Text = [string][char]0xE823; $g.Children[0].Foreground = (Br '#7A7A75')
    $g.Children[1].Foreground = (Br '#7A7A75'); $g.Children[1].FontFamily = 'Segoe UI'
    $g.Children[2].Text = 'Pending'; $g.Children[2].Foreground = (Br '#6A6A65')
    $row.Opacity = 0.6 }
}
function Start-Check {
  $script:CheckStart = Get-Date
  if (-not (Start-UserTask 'PSWloader Check')) { $u.HealthSub.Text = 'The check task is missing - run the PSWloader updater once.'; $script:AfterCheck = $null; return }
  $u.CheckNow.IsEnabled = $false; $u.FixNow.IsEnabled = $false; $u.CheckNow.Content = 'Checking ...'; Set-HealthPending; $script:CheckTimer.Start()
}
$u.CheckNow.Add_Click({ Start-Check })

# troubleshooting (EXPERIMENTAL): fresh check -> only when it finds fixable problems -> confirm -> SYSTEM task "PSWloader Fix"
$script:FixTimer = New-Object System.Windows.Threading.DispatcherTimer; $script:FixTimer.Interval = [TimeSpan]::FromMilliseconds(600)
$script:FixTimer.Add_Tick({
  $lines = Read-Progress $FixProgress $script:FixStart
  $last = @($lines | Where-Object { $_ -match '^\d+\t' } | Select-Object -Last 1)
  if ($last.Count) { $p = $last[0] -split "`t", 2; $u.HealthSub.Text = "Troubleshooting: $($p[0]) %  $($p[1])" }
  if ($lines -contains 'DONE' -or ((Get-Date) - $script:FixStart).TotalMinutes -gt 10) {
    $script:FixTimer.Stop(); $u.CheckNow.IsEnabled = $true
    $res = $null; try { $res = Get-Content $FixResult -Raw | ConvertFrom-Json } catch { }
    $script:Info = Get-Info; $script:Info.Cores = $script:Cores; Fill-Health $script:Info; Fill-Overview $script:Info
    $txt = if ($res) { (@($res.actions) | ForEach-Object { "$(if ($_.ok) { 'Done' } else { 'Not fixed' }): $($_.name)" }) -join "`n" } else { 'No result - see C:\ProgramData\PSWloader\logs\fix.log.' }
    if ($res -and $res.restart) {
      if ((Show-PswDialog -Bar 'PSWloader Dashboard' -Head 'Troubleshooting finished' -Text "$txt`n`nRestart Windows to finish the fixes." -One 'Restart now' -Two 'Later') -eq 1) { & shutdown.exe /r /t 5 /c 'PSWloader troubleshooting' } }
    else { Say 'Troubleshooting finished' "$txt`n`nIf a problem stays, create a support zip (Tools) and open an issue on GitHub." }
  }
})
$u.FixNow.Add_Click({
  $script:AfterCheck = {
    $fx = @(Get-Fixable $script:Info.Checks)
    if (-not $fx.Count) { Say 'Nothing to fix' 'The fresh check found no problem that troubleshooting can fix.'; return }
    $plan = ($fx | ForEach-Object { "-  $($_.name): $($_.detail)" }) -join "`n"
    if (-not (Ask 'Troubleshooting (experimental)' "The check found this - PSWloader tries to fix it:`n`n$plan`n`nDrivers may be installed again and some fixes need a restart." 'Fix now')) { return }
    $script:FixStart = Get-Date
    if (-not (Start-UserTask 'PSWloader Fix')) { Say 'Troubleshooting not available' 'The task is missing - run the PSWloader updater once.'; return }
    $u.CheckNow.IsEnabled = $false; $u.FixNow.IsEnabled = $false; $u.HealthSub.Text = 'Troubleshooting: starting ...'; $script:FixTimer.Start()
  }
  Start-Check
})

# updates: newest GitHub release vs version.json; with the beta switch on, pre-releases count too. The switch is kept per user
# (HKCU\Software\PSWloader BetaUpdates) and PSWUpdateCheck.ps1 - the sign-in check and "Download and install" - reads the same value.
function Compare-PswVersion([string]$A, [string]$B) {   # -1 / 0 / 1, $null when one of them is not a version; a release ranks above its own betas (as in PSWUpdate.ps1)
    $re = '^\s*[vV]?(\d+(?:\.\d+)*)(?:-(\S+))?\s*$'
    if ($A -notmatch $re -or $B -notmatch $re) { return $null }
    $p = foreach ($v in $A, $B) {
        [void]($v -match $re)
        $n = @($Matches[1].Split('.') | ForEach-Object { [int]$_ }); while ($n.Count -lt 3) { $n += 0 }
        , @($n, $Matches[2]) }
    for ($i = 0; $i -lt [Math]::Max($p[0][0].Count, $p[1][0].Count); $i++) {
        $x = if ($i -lt $p[0][0].Count) { $p[0][0][$i] } else { 0 }; $y = if ($i -lt $p[1][0].Count) { $p[1][0][$i] } else { 0 }
        if ($x -ne $y) { return [Math]::Sign($x - $y) } }
    if ($p[0][1] -eq $p[1][1]) { return 0 }
    if (-not $p[0][1]) { return 1 }; if (-not $p[1][1]) { return -1 }
    return [Math]::Sign([string]::CompareOrdinal($p[0][1], $p[1][1]))
}
$BetaKey = 'HKCU:\Software\PSWloader'
$u.UpdBeta.IsChecked = ((Get-ItemProperty -Path $BetaKey -Name BetaUpdates -ErrorAction SilentlyContinue).BetaUpdates -eq 1)
function Find-Update {
  $beta = [bool]$u.UpdBeta.IsChecked
  $u.UpdState.Text = 'Looking for updates ...'; $u.UpdInstall.IsEnabled = $false; $u.UpdInstall.Style = $root.Resources['Btn']
  $u.UpdLatestCap.Text = if ($beta) { 'NEWEST VERSION (BETA CHANNEL)' } else { 'LATEST RELEASE' }
  try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $hdr = @{ 'User-Agent' = 'PSWloader-Dashboard' }
    if ($beta) {
      $all = @(Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases?per_page=30" -Headers $hdr -TimeoutSec 15 | Where-Object { $_.tag_name -and -not $_.draft })
      $rel = $null; foreach ($r in $all) { if (-not $rel -or (Compare-PswVersion ([string]$r.tag_name) ([string]$rel.tag_name)) -gt 0) { $rel = $r } }
    } else { $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers $hdr -TimeoutSec 15 }
    if (-not $rel) { $u.UpdLatest.Text = '-'; $u.UpdLatestDate.Text = ''; $u.UpdState.Text = 'No release found.'; return }
    $u.UpdLatest.Text = "$($rel.tag_name)$(if ($rel.prerelease) { '  (beta)' })"; $u.UpdLatestDate.Text = try { 'published ' + ([datetime]$rel.published_at).ToString('dd.MM.yyyy') } catch { '' }
    $cmp = Compare-PswVersion ([string]$rel.tag_name) ([string]$script:Info.Version)
    if ($null -ne $cmp -and $cmp -gt 0) {   # only a newer version can be installed
      $u.UpdState.Text = "PSWloader $($rel.tag_name) is available$(if ($rel.prerelease) { ' - a beta version, experimental, at your own risk' })."; $u.UpdInstall.IsEnabled = $true; $u.UpdInstall.Style = $root.Resources['Primary'] }
    elseif ($null -eq $cmp) { $u.UpdState.Text = "The installed version ($($script:Info.Version)) could not be compared with $($rel.tag_name)." }
    else { $u.UpdState.Text = if ($beta) { 'Nothing newer than the installed version, also not in the beta channel.' } else { 'PSWloader is up to date.' } }
  } catch { $u.UpdState.Text = 'GitHub could not be reached - check the network connection.' }
}
$u.UpdCheck.Add_Click({ Find-Update })
# automatic update check at sign-in: per user (HKCU AutoUpdateCheck); without it the machine-wide updater.json counts
$autoVal = (Get-ItemProperty -Path $BetaKey -Name AutoUpdateCheck -ErrorAction SilentlyContinue).AutoUpdateCheck
if ($null -eq $autoVal) { $autoVal = 1; try { $j = Get-Content "$Data\updater.json" -Raw | ConvertFrom-Json; if ($null -ne $j.autoCheck -and -not [bool]$j.autoCheck) { $autoVal = 0 } } catch { } }
$u.UpdAuto.IsChecked = ([int]$autoVal -ne 0)
$u.UpdAuto.Add_Click({
  try { if (-not (Test-Path $BetaKey)) { New-Item -Path $BetaKey -Force | Out-Null }
        Set-ItemProperty -Path $BetaKey -Name AutoUpdateCheck -Value ([int][bool]$this.IsChecked) -Type DWord } catch { } })
# start with Windows: per user (HKCU DashboardAutostart, 0 = off); the Run entry itself belongs to PSWSetup (all users)
$u.ToolAutostart.IsChecked = ((Get-ItemProperty -Path $BetaKey -Name DashboardAutostart -ErrorAction SilentlyContinue).DashboardAutostart -ne 0)
$u.ToolAutostart.Add_Click({
  try { if (-not (Test-Path $BetaKey)) { New-Item -Path $BetaKey -Force | Out-Null }
        Set-ItemProperty -Path $BetaKey -Name DashboardAutostart -Value ([int][bool]$this.IsChecked) -Type DWord } catch { } })
$u.UpdBeta.Add_Click({
  try { if (-not (Test-Path $BetaKey)) { New-Item -Path $BetaKey -Force | Out-Null }
        Set-ItemProperty -Path $BetaKey -Name BetaUpdates -Value ([int][bool]$this.IsChecked) -Type DWord } catch { }
  Find-Update })
$u.UpdInstall.Add_Click({
  $chk = "$Data\updater\PSWUpdateCheck.ps1"
  if (Test-Path $chk) { Start-Process powershell.exe -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$chk`" -Force" }
  else { Start-Process "https://github.com/$Repo/releases/latest" } })
$u.UpdNotes.Add_Click({ Start-Process "https://github.com/$Repo/releases" })

# ---------------------------------------------------------------- power & fan (v1.6: interactive gaming curve editor + live apply)
$script:PwrPreset = 'standard'
$script:FanPreset = 1
$script:LiveSoC = $null; $script:LiveFan = $null
$script:FanCurvePresets = @{
  0 = @{ t = @(55,68,72,76,80,84,87,89); d = @(10,10,10,15,24,42,68,100) }   # Silent (Efficiency)
  1 = @{ t = @(55,68,72,76,80,84,87,89); d = @(10,10,14,22,34,46,68,100) }   # Balanced (Standard)
  2 = @{ t = @(55,68,72,76,80,84,87,89); d = @(10,10,16,26,38,50,72,100) }   # Performance (OC)
}
$script:FanTemps  = [System.Collections.Generic.List[int]]::new([int[]]@(55,68,72,76,80,84,87,89))
$script:FanDuties = [System.Collections.Generic.List[int]]::new([int[]]@(10,10,14,22,34,46,68,100))
$script:FanDragIdx = -1
$script:FanHoverIdx = -1
$script:FanDirty = $false

function Set-FanPoints([int[]]$Temps, [int[]]$Duties) {
  $script:FanTemps.Clear(); $script:FanDuties.Clear()
  $n = [Math]::Min($Temps.Count, $Duties.Count)
  for ($i = 0; $i -lt $n; $i++) { [void]$script:FanTemps.Add([int]$Temps[$i]); [void]$script:FanDuties.Add([int]$Duties[$i]) }
}
function Set-PowerCardStyle($Card, [bool]$Active) {
  $Card.Background = (Br $(if ($Active) { '#1D2B3D' } else { '#242424' }))
  $Card.BorderBrush = (Br $(if ($Active) { $Blue } else { '#343434' }))
  $Card.BorderThickness = if ($Active) { '1.5' } else { '1' }
  $sp = $Card.Child
  if ($sp -and $sp.Children.Count -ge 2) {
    $sp.Children[0].Foreground = (Br $(if ($Active) { '#FFFFFF' } else { '#E6E6E2' }))
    $sp.Children[1].Foreground = (Br $(if ($Active) { '#BBD6F7' } else { '#8E8E88' }))
  }
}
function Update-PowerFanButtons {
  Set-PowerCardStyle $u.PwrEff ($script:PwrPreset -eq 'efficiency')
  Set-PowerCardStyle $u.PwrStd ($script:PwrPreset -eq 'standard')
  Set-PowerCardStyle $u.PwrOc  ($script:PwrPreset -eq 'oc')
  $u.FanSilent.Style   = $root.Resources[$(if ($script:FanPreset -eq 0) { 'Primary' } else { 'Btn' })]
  $u.FanBalanced.Style = $root.Resources[$(if ($script:FanPreset -eq 1) { 'Primary' } else { 'Btn' })]
  $u.FanPerf.Style     = $root.Resources[$(if ($script:FanPreset -eq 2) { 'Primary' } else { 'Btn' })]
  $u.FanCustom.Style   = $root.Resources[$(if ($script:FanPreset -eq 3) { 'Primary' } else { 'Btn' })]
}
function Get-FanPlotMetrics {
  $w = $u.FanCanvas.ActualWidth; $h = $u.FanCanvas.ActualHeight
  if ($w -lt 80) { $w = 780 }; if ($h -lt 80) { $h = 300 }
  $ml = 42.0; $mr = 18.0; $mt = 16.0; $mb = 28.0
  @{ w = $w; h = $h; ml = $ml; mr = $mr; mt = $mt; mb = $mb; pw = ($w - $ml - $mr); ph = ($h - $mt - $mb); tMin = 35.0; tMax = 95.0 }
}
function Draw-FanCurve {
  $c = $u.FanCanvas; $c.Children.Clear()
  $m = Get-FanPlotMetrics
  $ml = $m.ml; $mt = $m.mt; $pw = $m.pw; $ph = $m.ph; $tMin = $m.tMin; $tMax = $m.tMax
  $xOf = { param($tc) $ml + (([Math]::Max($tMin, [Math]::Min($tMax, [double]$tc)) - $tMin) / ($tMax - $tMin)) * $pw }
  $yOf = { param($dp) $mt + (1.0 - ([Math]::Max(0.0, [Math]::Min(100.0, [double]$dp)) / 100.0)) * $ph }
  $cnt = $script:FanTemps.Count; if ($cnt -lt 2) { return }

  # Thermal safety zone (>= 90 C)
  $x90 = & $xOf 90
  $safe = New-Object System.Windows.Shapes.Rectangle
  $safe.Width = [Math]::Max(0, ($ml + $pw) - $x90); $safe.Height = $ph; $safe.Fill = (Br '#1AE5534B')
  [System.Windows.Controls.Canvas]::SetLeft($safe, $x90); [System.Windows.Controls.Canvas]::SetTop($safe, $mt)
  [void]$c.Children.Add($safe)

  # Horizontal grid (0..100%)
  foreach ($dp in 0, 20, 40, 60, 80, 100) {
    $y = & $yOf $dp
    $ln = New-Object System.Windows.Shapes.Line; $ln.X1 = $ml; $ln.X2 = $ml + $pw; $ln.Y1 = $y; $ln.Y2 = $y
    $ln.Stroke = (Br $(if ($dp -eq 0) { '#383838' } else { '#242424' })); $ln.StrokeThickness = 1; [void]$c.Children.Add($ln)
    $lb = T "$dp%" 10.5 '#7E7E78'; $lb.Width = 34; $lb.TextAlignment = 'Right'
    [System.Windows.Controls.Canvas]::SetLeft($lb, 2); [System.Windows.Controls.Canvas]::SetTop($lb, $y - 7); [void]$c.Children.Add($lb)
  }
  # Vertical grid (40..90 C)
  foreach ($tc in 40, 50, 60, 70, 80, 90) {
    $x = & $xOf $tc
    $ln = New-Object System.Windows.Shapes.Line; $ln.X1 = $x; $ln.X2 = $x; $ln.Y1 = $mt; $ln.Y2 = $mt + $ph
    $ln.Stroke = (Br $(if ($tc -eq 90) { '#663432' } else { '#242424' })); $ln.StrokeThickness = 1; [void]$c.Children.Add($ln)
    $lb = T "$tc$([char]0xB0)C" 10.5 $(if ($tc -eq 90) { '#E06C65' } else { '#7E7E78' })
    [System.Windows.Controls.Canvas]::SetLeft($lb, $x - 14); [System.Windows.Controls.Canvas]::SetTop($lb, $mt + $ph + 6); [void]$c.Children.Add($lb)
  }
  # Curve fill + line
  $pts = New-Object System.Windows.Media.PointCollection
  $fill = New-Object System.Windows.Media.PointCollection
  $x0 = & $xOf $tMin; $y0 = & $yOf $script:FanDuties[0]
  [void]$pts.Add((New-Object System.Windows.Point($x0, $y0)))
  [void]$fill.Add((New-Object System.Windows.Point($x0, ($mt + $ph))))
  [void]$fill.Add((New-Object System.Windows.Point($x0, $y0)))
  for ($i = 0; $i -lt $cnt; $i++) {
    $pt = New-Object System.Windows.Point((& $xOf $script:FanTemps[$i]), (& $yOf $script:FanDuties[$i]))
    [void]$pts.Add($pt); [void]$fill.Add($pt)
  }
  $xe = & $xOf $tMax; $ye = & $yOf 100
  [void]$pts.Add((New-Object System.Windows.Point($xe, $ye)))
  [void]$fill.Add((New-Object System.Windows.Point($xe, $ye)))
  [void]$fill.Add((New-Object System.Windows.Point($xe, ($mt + $ph))))
  $pg = New-Object System.Windows.Shapes.Polygon; $pg.Points = $fill; $pg.Fill = (Br '#283A86E0'); [void]$c.Children.Add($pg)
  $pl = New-Object System.Windows.Shapes.Polyline; $pl.Points = $pts; $pl.Stroke = (Br $Blue); $pl.StrokeThickness = 2.5; $pl.StrokeLineJoin = 'Round'; [void]$c.Children.Add($pl)

  # Live SoC / Fan crosshair + dot
  if ($null -ne $script:LiveSoC -and $null -ne $script:LiveFan) {
    $lx = & $xOf $script:LiveSoC; $ly = & $yOf $script:LiveFan
    $vl = New-Object System.Windows.Shapes.Line; $vl.X1 = $lx; $vl.X2 = $lx; $vl.Y1 = $mt; $vl.Y2 = $mt + $ph
    $vl.Stroke = (Br '#444EC97A'); $vl.StrokeThickness = 1; $vl.StrokeDashArray = [System.Windows.Media.DoubleCollection]@([double]3, [double]3); [void]$c.Children.Add($vl)
    $hl = New-Object System.Windows.Shapes.Line; $hl.X1 = $ml; $hl.X2 = $ml + $pw; $hl.Y1 = $ly; $hl.Y2 = $ly
    $hl.Stroke = (Br '#444EC97A'); $hl.StrokeThickness = 1; $hl.StrokeDashArray = [System.Windows.Media.DoubleCollection]@([double]3, [double]3); [void]$c.Children.Add($hl)
    $dot = New-Object System.Windows.Shapes.Ellipse; $dot.Width = 10; $dot.Height = 10
    $dot.Fill = (Br '#4EC97A'); $dot.Stroke = (Br '#121212'); $dot.StrokeThickness = 2
    [System.Windows.Controls.Canvas]::SetLeft($dot, $lx - 5); [System.Windows.Controls.Canvas]::SetTop($dot, $ly - 5); [void]$c.Children.Add($dot)
  }

  # Draggable control points (last point locked at 100% duty for thermal safety)
  $last = $cnt - 1
  for ($i = 0; $i -lt $cnt; $i++) {
    $px = & $xOf $script:FanTemps[$i]; $py = & $yOf $script:FanDuties[$i]
    $active = ($i -eq $script:FanDragIdx -or $i -eq $script:FanHoverIdx)
    if ($active) {
      $halo = New-Object System.Windows.Shapes.Ellipse; $halo.Width = 22; $halo.Height = 22
      $halo.Fill = (Br $(if ($i -eq $last) { '#33E5534B' } else { '#383A86E0' }))
      [System.Windows.Controls.Canvas]::SetLeft($halo, $px - 11); [System.Windows.Controls.Canvas]::SetTop($halo, $py - 11); [void]$c.Children.Add($halo)
    }
    $sz = if ($active) { 13.0 } else { 11.0 }
    $el = New-Object System.Windows.Shapes.Ellipse; $el.Width = $sz; $el.Height = $sz
    $el.Fill = (Br $(if ($i -eq $last) { '#E5534B' } elseif ($active) { '#FFFFFF' } else { '#EAEAE5' }))
    $el.Stroke = (Br $(if ($i -eq $last) { '#FF8A84' } else { $Blue })); $el.StrokeThickness = 2.2
    [System.Windows.Controls.Canvas]::SetLeft($el, $px - $sz / 2); [System.Windows.Controls.Canvas]::SetTop($el, $py - $sz / 2)
    [void]$c.Children.Add($el)
  }

  # Floating callout badge for active/hovered point
  $ti = if ($script:FanDragIdx -ge 0) { $script:FanDragIdx } else { $script:FanHoverIdx }
  if ($ti -ge 0 -and $ti -lt $cnt) {
    $px = & $xOf $script:FanTemps[$ti]; $py = & $yOf $script:FanDuties[$ti]
    $tipTxt = "P$($ti + 1)/$cnt : $($script:FanTemps[$ti]) $([char]0xB0)C  $([char]0xB7)  $($script:FanDuties[$ti]) %$(if ($ti -eq $last) { ' (max)' })"
    $tip = New-Object System.Windows.Controls.Border; $tip.Background = (Br '#E6222222'); $tip.BorderBrush = (Br $Blue)
    $tip.BorderThickness = 1; $tip.CornerRadius = '4'; $tip.Padding = '8,3'
    $tip.Child = (T $tipTxt 11.5 '#FFFFFF' 'Segoe UI Semibold')
    $tx = [Math]::Max($ml + 4, [Math]::Min($ml + $pw - 165, $px - 70))
    $ty = if ($py -lt ($mt + 38)) { $py + 14 } else { $py - 32 }
    [System.Windows.Controls.Canvas]::SetLeft($tip, $tx); [System.Windows.Controls.Canvas]::SetTop($tip, $ty)
    [void]$c.Children.Add($tip)
  }
}
function Find-NearestFanPoint([double]$X, [double]$Y) {
  $m = Get-FanPlotMetrics; $best = -1; $bestD = 22.0; $cnt = $script:FanTemps.Count
  for ($i = 0; $i -lt $cnt; $i++) {
    $px = $m.ml + (([double]$script:FanTemps[$i] - $m.tMin) / ($m.tMax - $m.tMin)) * $m.pw
    $py = $m.mt + (1.0 - ([double]$script:FanDuties[$i] / 100.0)) * $m.ph
    $dist = [Math]::Sqrt(($X - $px) * ($X - $px) + ($Y - $py) * ($Y - $py))
    if ($dist -lt $bestD) { $bestD = $dist; $best = $i }
  }
  $best
}
function Move-FanPoint([int]$Idx, [double]$X, [double]$Y) {
  $cnt = $script:FanTemps.Count; $last = $cnt - 1
  if ($Idx -lt 0 -or $Idx -gt $last) { return }
  $m = Get-FanPlotMetrics
  $tc = [int][Math]::Round($m.tMin + (($X - $m.ml) / $m.pw) * ($m.tMax - $m.tMin))
  $dp = [int][Math]::Round((1.0 - (($Y - $m.mt) / $m.ph)) * 100.0)
  $tLo = if ($Idx -eq 0) { 35 } else { $script:FanTemps[$Idx - 1] + 1 }
  $tHi = if ($Idx -eq $last) { 89 } else { $script:FanTemps[$Idx + 1] - 1 }
  $script:FanTemps[$Idx] = [Math]::Max($tLo, [Math]::Min($tHi, $tc))
  if ($Idx -eq $last) { $script:FanDuties[$last] = 100 }
  else {
    $dp = [Math]::Max(10, [Math]::Min(100, $dp))
    $script:FanDuties[$Idx] = $dp
    for ($k = $Idx + 1; $k -lt $cnt; $k++) { if ($script:FanDuties[$k] -lt $dp) { $script:FanDuties[$k] = $dp } }
    for ($k = $Idx - 1; $k -ge 0; $k--) { if ($script:FanDuties[$k] -gt $dp) { $script:FanDuties[$k] = $dp } }
    $script:FanDuties[$last] = 100
  }
  $script:FanPreset = 3; $script:FanDirty = $true
  Update-PowerFanButtons; Draw-FanCurve
  $u.PwrStatus.Text = "Custom ($cnt pts) - P$($Idx + 1): $($script:FanTemps[$Idx]) $([char]0xB0)C -> $($script:FanDuties[$Idx]) % - click Apply to save."
}
function Add-FanPointAt([double]$X, [double]$Y) {
  $cnt = $script:FanTemps.Count
  if ($cnt -ge 8) { $u.PwrStatus.Text = 'Maximum 8 points reached - right-click any point to remove one first.'; return -1 }
  $m = Get-FanPlotMetrics
  if ($X -lt $m.ml -or $X -gt ($m.ml + $m.pw) -or $Y -lt $m.mt -or $Y -gt ($m.mt + $m.ph)) { return -1 }
  $tc = [int][Math]::Max(35, [Math]::Min(88, [Math]::Round($m.tMin + (($X - $m.ml) / $m.pw) * ($m.tMax - $m.tMin))))
  $dp = [int][Math]::Max(10, [Math]::Min(100, [Math]::Round((1.0 - (($Y - $m.mt) / $m.ph)) * 100.0)))
  # Avoid exact duplicate temperature
  if ($script:FanTemps.Contains($tc)) {
    if (-not $script:FanTemps.Contains($tc + 1) -and ($tc + 1) -lt $script:FanTemps[$cnt - 1]) { $tc++ }
    elseif (-not $script:FanTemps.Contains($tc - 1) -and ($tc - 1) -ge 35) { $tc-- }
    else { return -1 }
  }
  $ins = 0; while ($ins -lt ($cnt - 1) -and $script:FanTemps[$ins] -lt $tc) { $ins++ }
  $script:FanTemps.Insert($ins, $tc); $script:FanDuties.Insert($ins, $dp)
  $cnt = $script:FanTemps.Count
  for ($k = $ins + 1; $k -lt $cnt; $k++) { if ($script:FanDuties[$k] -lt $dp) { $script:FanDuties[$k] = $dp } }
  for ($k = $ins - 1; $k -ge 0; $k--) { if ($script:FanDuties[$k] -gt $dp) { $script:FanDuties[$k] = $dp } }
  $script:FanDuties[$cnt - 1] = 100
  $script:FanPreset = 3; $script:FanDirty = $true
  Update-PowerFanButtons; Draw-FanCurve
  $u.PwrStatus.Text = "Added point P$($ins + 1) ($tc $([char]0xB0)C -> $dp %) - click Apply to save."
  $ins
}
function Remove-FanPointAt([int]$Idx) {
  $cnt = $script:FanTemps.Count
  if ($cnt -le 2) { $u.PwrStatus.Text = 'At least 2 points are required.'; return }
  if ($Idx -lt 0 -or $Idx -ge ($cnt - 1)) { $u.PwrStatus.Text = 'The 100 % thermal safety point cannot be removed.'; return }
  $script:FanTemps.RemoveAt($Idx); $script:FanDuties.RemoveAt($Idx)
  $script:FanHoverIdx = -1; $script:FanPreset = 3; $script:FanDirty = $true
  Update-PowerFanButtons; Draw-FanCurve
  $u.PwrStatus.Text = "Removed point ($($script:FanTemps.Count) pts left) - click Apply to save."
}
function Export-FanCurve8 {
  # Resample 2..8 user points cleanly to the 8 monotonic points ps5pwr.sys expects (40..89 C, 10..100%)
  $cnt = $script:FanTemps.Count
  $tOut = New-Object int[] 8; $dOut = New-Object int[] 8
  if ($cnt -eq 8) {
    for ($i = 0; $i -lt 8; $i++) { $tOut[$i] = [Math]::Max(40, [Math]::Min(89, $script:FanTemps[$i])); $dOut[$i] = $script:FanDuties[$i] }
  } else {
    $t0 = [double]$script:FanTemps[0]; $tN = [double]$script:FanTemps[$cnt - 1]
    for ($i = 0; $i -lt 8; $i++) {
      $tc = if ($i -eq 0) { $t0 } elseif ($i -eq 7) { $tN } else { $t0 + ($tN - $t0) * ($i / 7.0) }
      $j = 0; while ($j -lt ($cnt - 2) -and $script:FanTemps[$j + 1] -lt $tc) { $j++ }
      $xa = [double]$script:FanTemps[$j]; $xb = [double]$script:FanTemps[$j + 1]
      $ya = [double]$script:FanDuties[$j]; $yb = [double]$script:FanDuties[$j + 1]
      $f = if ($xb -gt $xa) { [Math]::Max(0.0, [Math]::Min(1.0, ($tc - $xa) / ($xb - $xa))) } else { 0.0 }
      $tOut[$i] = [int][Math]::Max(40, [Math]::Min(89, [Math]::Round($tc)))
      $dOut[$i] = [int][Math]::Max(10, [Math]::Min(100, [Math]::Round($ya + $f * ($yb - $ya))))
    }
  }
  $dOut[7] = 100
  @($tOut + $dOut)
}
function Load-PowerFanState {
  $pwrKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters'
  try {
    $fp = (Get-ItemProperty -Path $pwrKey -Name 'FanPreset' -EA SilentlyContinue).FanPreset
    if ($null -ne $fp -and [int]$fp -ge 0 -and [int]$fp -le 3) { $script:FanPreset = [int]$fp }
    $bin = (Get-ItemProperty -Path $pwrKey -Name 'FanCustomCurve' -EA SilentlyContinue).FanCustomCurve
    if ($script:FanPreset -eq 3 -and $bin -and $bin.Length -eq 16) {
      Set-FanPoints @(0..7 | ForEach-Object { [int]$bin[$_] }) @(8..15 | ForEach-Object { [int]$bin[$_] })
    } elseif ($script:FanCurvePresets.ContainsKey($script:FanPreset)) {
      Set-FanPoints $script:FanCurvePresets[$script:FanPreset].t $script:FanCurvePresets[$script:FanPreset].d
    }
  } catch { }
  try {
    $stFile = "$DashData\power-state.json"
    if (Test-Path -LiteralPath $stFile) {
      $st = Get-Content -LiteralPath $stFile -Raw | ConvertFrom-Json
      if ($st.powerPreset -in 'efficiency','standard','oc') { $script:PwrPreset = [string]$st.powerPreset }
      if ($script:FanPreset -eq 3 -and $st.customTemps -and $st.customDuties -and @($st.customTemps).Count -ge 2) {
        Set-FanPoints @($st.customTemps | ForEach-Object { [int]$_ }) @($st.customDuties | ForEach-Object { [int]$_ })
      }
    } else {
      $tmLog = 'C:\ps5-work\thermal-manager.jsonl'
      if (Test-Path -LiteralPath $tmLog) {
        $pl = Select-String -Path $tmLog -Pattern '"event":"preset"' | Select-Object -Last 1
        if ($pl -and $pl.Line -match '"preset":"(efficiency|standard|oc)"') { $script:PwrPreset = $Matches[1] }
      }
    }
  } catch { }
  Update-PowerFanButtons; Draw-FanCurve
}
function Apply-PowerFan {
  $u.PwrStatus.Text = 'Applying ...'
  $reqDir = "$Data\power-req"; $reqFile = "$reqDir\request.json"
  try { if (-not (Test-Path -LiteralPath $reqDir)) { New-Item -ItemType Directory -Force $reqDir | Out-Null } } catch { }
  $payload = [ordered]@{
    powerPreset  = $script:PwrPreset
    fanPreset    = $script:FanPreset
    customCurve  = @(Export-FanCurve8)
    customTemps  = @($script:FanTemps.ToArray())
    customDuties = @($script:FanDuties.ToArray())
  }
  try { Set-Content -LiteralPath $reqFile -Value ($payload | ConvertTo-Json -Compress) -Encoding UTF8 }
  catch { $u.PwrStatus.Text = "Could not write request: $($_.Exception.Message)"; return }
  $pLabel = switch ($script:PwrPreset) { 'efficiency' { 'Efficiency' } 'oc' { 'Power' } default { 'Standard' } }
  $fLabel = @('Silent','Balanced','Performance','Custom')[$script:FanPreset]
  if (Start-UserTask 'PSWloader Power') {
    $script:FanDirty = $false
    $u.PwrStatus.Text = "Active: $pLabel  $([char]0xB7)  Fan: $fLabel (applied live)."
  } else {
    $helper = @("$PSScriptRoot\PSWPowerApply.ps1", "$env:ProgramFiles\PSWloader\PSWPowerApply.ps1") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($helper) {
      try {
        Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -Wait -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$helper`" -RequestFile `"$reqFile`""
        $script:FanDirty = $false
        $u.PwrStatus.Text = "Active: $pLabel  $([char]0xB7)  Fan: $fLabel (applied live)."
      } catch { $u.PwrStatus.Text = 'Cancelled or administrator rights required.' }
    } else { $u.PwrStatus.Text = 'PSWloader Power task missing - run the PSWloader updater once.' }
  }
}
$u.FanCanvas.Add_SizeChanged({ if ($u.PagePower.Visibility -eq 'Visible') { Draw-FanCurve } })
$u.FanCanvas.Add_MouseLeftButtonDown({
  $_.Handled = $true
  $pos = $_.GetPosition($u.FanCanvas)
  $idx = Find-NearestFanPoint $pos.X $pos.Y
  if ($idx -lt 0) { $idx = Add-FanPointAt $pos.X $pos.Y }
  if ($idx -ge 0) {
    $script:FanDragIdx = $idx
    [void]$u.FanCanvas.CaptureMouse()
    Draw-FanCurve
  }
})
$u.FanCanvas.Add_MouseRightButtonDown({
  $_.Handled = $true
  $pos = $_.GetPosition($u.FanCanvas)
  $idx = Find-NearestFanPoint $pos.X $pos.Y
  if ($idx -ge 0) { Remove-FanPointAt $idx }
})
$u.FanCanvas.Add_MouseMove({
  $pos = $_.GetPosition($u.FanCanvas)
  if ($script:FanDragIdx -ge 0) {
    $_.Handled = $true
    Move-FanPoint $script:FanDragIdx $pos.X $pos.Y
  } else {
    $h = Find-NearestFanPoint $pos.X $pos.Y
    if ($h -ne $script:FanHoverIdx) {
      $script:FanHoverIdx = $h
      $u.FanCanvas.Cursor = if ($h -ge 0) { [System.Windows.Input.Cursors]::Hand } else { [System.Windows.Input.Cursors]::Cross }
      Draw-FanCurve
    }
  }
})
$u.FanCanvas.Add_MouseLeftButtonUp({
  if ($script:FanDragIdx -ge 0) {
    $_.Handled = $true
    $script:FanDragIdx = -1
    $u.FanCanvas.ReleaseMouseCapture()
    Draw-FanCurve
  }
})
$u.FanCanvas.Add_MouseLeave({
  if ($script:FanDragIdx -lt 0 -and $script:FanHoverIdx -ge 0) {
    $script:FanHoverIdx = -1; Draw-FanCurve
  }
})
$u.PwrEff.Add_MouseLeftButtonDown({ $_.Handled = $true; $script:PwrPreset = 'efficiency'; if ($script:FanPreset -ne 3) { $script:FanPreset = 0; Set-FanPoints $script:FanCurvePresets[0].t $script:FanCurvePresets[0].d }; Update-PowerFanButtons; Draw-FanCurve; Apply-PowerFan })
$u.PwrStd.Add_MouseLeftButtonDown({ $_.Handled = $true; $script:PwrPreset = 'standard'; if ($script:FanPreset -ne 3) { $script:FanPreset = 1; Set-FanPoints $script:FanCurvePresets[1].t $script:FanCurvePresets[1].d }; Update-PowerFanButtons; Draw-FanCurve; Apply-PowerFan })
$u.PwrOc.Add_MouseLeftButtonDown({ $_.Handled = $true; $script:PwrPreset = 'oc'; if ($script:FanPreset -ne 3) { $script:FanPreset = 2; Set-FanPoints $script:FanCurvePresets[2].t $script:FanCurvePresets[2].d }; Update-PowerFanButtons; Draw-FanCurve; Apply-PowerFan })
$u.FanSilent.Add_Click({ $script:FanPreset = 0; Set-FanPoints $script:FanCurvePresets[0].t $script:FanCurvePresets[0].d; Update-PowerFanButtons; Draw-FanCurve; Apply-PowerFan })
$u.FanBalanced.Add_Click({ $script:FanPreset = 1; Set-FanPoints $script:FanCurvePresets[1].t $script:FanCurvePresets[1].d; Update-PowerFanButtons; Draw-FanCurve; Apply-PowerFan })
$u.FanPerf.Add_Click({ $script:FanPreset = 2; Set-FanPoints $script:FanCurvePresets[2].t $script:FanCurvePresets[2].d; Update-PowerFanButtons; Draw-FanCurve; Apply-PowerFan })
$u.FanCustom.Add_Click({ $script:FanPreset = 3; Update-PowerFanButtons; Draw-FanCurve; $u.PwrStatus.Text = 'Drag points, click to add (max 8), right-click to remove, then Apply.' })
$u.FanReset.Add_Click({ $p = if ($script:PwrPreset -eq 'efficiency') { 0 } elseif ($script:PwrPreset -eq 'oc') { 2 } else { 1 }; $script:FanPreset = $p; Set-FanPoints $script:FanCurvePresets[$p].t $script:FanCurvePresets[$p].d; Update-PowerFanButtons; Draw-FanCurve; Apply-PowerFan })
$u.FanApply.Add_Click({ Apply-PowerFan })
Load-PowerFanState

$u.NavOverview.Add_Checked({ Show-Page 'Overview' }); $u.NavPower.Add_Checked({ Show-Page 'Power' }); $u.NavHealth.Add_Checked({ Show-Page 'Health' })
$u.NavTools.Add_Checked({ Show-Page 'Tools' }); $u.NavUpdates.Add_Checked({ Show-Page 'Updates' })
$u.StatusBanner.Add_MouseLeftButtonUp({ Show-Page 'Health' })

function Show-Info {   # everything that depends on the data read from the PC
  if (-not $script:TData) { $script:TData = [int]((Get-Date) - $script:T0).TotalMilliseconds }
  Fill-Overview $script:Info; Fill-Health $script:Info
  $u.UpdInstalled.Text = $script:Info.Version; $u.UpdFirmware.Text = $script:Info.Firmware
}
$script:T0 = [Diagnostics.Process]::GetCurrentProcess().StartTime   # start times for -SelfTest
Fill-Tools; Show-Page $Page
if ($Preview) { $script:Info = Get-Info; $script:Info.Cores = [int]((Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum); Show-Info }

if ($Preview) {
  # preview picture only: a sample history so the graphs are visible (the real window reads live values)
  $rnd = New-Object Random 7
  for ($k = 0; $k -lt 60; $k++) { Push $script:Hist.cpu (12 + 10 * [Math]::Sin($k / 6) + $rnd.Next(0, 9)); Push $script:Hist.gpu ([Math]::Max(0, 30 * [Math]::Sin($k / 9) + $rnd.Next(0, 12)))
                                  Push $script:Hist.ram (41 + $rnd.Next(0, 3)); Push $script:Hist.temp (60 + 4 * [Math]::Sin($k / 12) + $rnd.NextDouble()) }
  $root.Measure([System.Windows.Size]::new([double]::PositiveInfinity, [double]::PositiveInfinity)); $root.Arrange([System.Windows.Rect]::new($root.DesiredSize)); $root.UpdateLayout()
  Update-Live -Sample; $root.UpdateLayout()
  $bmp = [System.Windows.Media.Imaging.RenderTargetBitmap]::new([int]($root.ActualWidth * 2), [int]($root.ActualHeight * 2), 192, 192, [System.Windows.Media.PixelFormats]::Pbgra32)
  $bmp.Render($root); $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder; $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bmp))
  $fs = [IO.File]::Create($Preview); $enc.Save($fs); $fs.Close(); exit 0
}
$w = New-Object System.Windows.Window
$w.Title = 'PSWloader Dashboard'; $w.Content = $root; $w.SizeToContent = 'WidthAndHeight'; $w.ResizeMode = 'CanMinimize'; $w.WindowStartupLocation = 'CenterScreen'
# on a small desktop (a TV at 720p, or 4K at 300 % scaling = 1280 x 720) the whole window is scaled down to fit above the taskbar
function Update-WindowScale {
  $wa = [System.Windows.SystemParameters]::WorkArea; $fit = [Math]::Min(1.0, [Math]::Min(($wa.Width - 16) / 1100, ($wa.Height - 16) / 720))
  $root.LayoutTransform = if ($fit -lt 1 -and $fit -gt 0.3) { New-Object System.Windows.Media.ScaleTransform($fit, $fit) } else { $null }
}
Update-WindowScale
$ico = "$Data\icons\psw-dashboard.ico"; if (Test-Path $ico) { try { $w.Icon = [System.Windows.Media.Imaging.BitmapFrame]::Create([Uri]$ico) } catch { } }
$script:LiveTimer = New-Object System.Windows.Threading.DispatcherTimer; $script:LiveTimer.Interval = [TimeSpan]::FromSeconds(1)
$script:LiveTimer.Add_Tick({ if ($u.PageOverview.Visibility -eq 'Visible' -and $w.WindowState -ne 'Minimized') { Update-Live } else { $null = Update-Live } })
# the window comes first; three background runspaces read the data (info, performance counters, number of cores) and a
# timer puts each result on the pages when it is there
$u.HealthSub.Text = 'Reading ...'; $u.StatusHead.Text = 'Reading ...'; $u.SideState.Text = 'Reading ...'
$script:Bg = @{}; $script:LastChecksTicks = [int64]0; $script:DynPending = $false
function Start-Bg([string]$Name, [string]$Script, [object[]]$BgArgs = @()) {
  if ($script:Bg.ContainsKey($Name)) { return }
  $ps = [powershell]::Create(); [void]$ps.AddScript($Script); foreach ($a in $BgArgs) { [void]$ps.AddArgument($a) }
  $script:Bg[$Name] = @{ Ps = $ps; Handle = $ps.BeginInvoke() }
  if (-not $script:BgTimer.IsEnabled) { $script:BgTimer.Start() } }
function Start-DynRefresh {
  if (-not $script:Info -or $script:Bg.ContainsKey('dyn')) { return }
  Start-Bg 'dyn' $DynScript @($ChecksFile, $script:LastChecksTicks) }
$script:DynTimer = New-Object System.Windows.Threading.DispatcherTimer; $script:DynTimer.Interval = [TimeSpan]::FromSeconds(5)
$script:DynTimer.Add_Tick({
  if ($script:Info -and $w.WindowState -ne 'Minimized') { Start-DynRefresh } })
$script:BgTimer = New-Object System.Windows.Threading.DispatcherTimer; $script:BgTimer.Interval = [TimeSpan]::FromMilliseconds(120)
$script:BgTimer.Add_Tick({
  foreach ($n in @($script:Bg.Keys)) {
    $b = $script:Bg[$n]; if (-not $b.Handle.IsCompleted) { continue }
    $res = $null; try { $res = @($b.Ps.EndInvoke($b.Handle))[0] } catch { }; $b.Ps.Dispose(); $script:Bg.Remove($n)
    if ($n -eq 'info') {
      $script:Info = if ($res) { $res } else { Get-Info }
      if ($script:Cores) { $script:Info.Cores = $script:Cores }
      $cf0 = Get-Item -LiteralPath $ChecksFile -ErrorAction SilentlyContinue
      if ($cf0) { $script:LastChecksTicks = $cf0.LastWriteTimeUtc.Ticks }
      Show-Info
      # a fresh check in the background when the last one is older than an hour
      if (-not $cf0 -or $cf0.LastWriteTime -lt (Get-Date).AddHours(-1)) { Start-Check }
    } elseif ($n -eq 'counters') { $script:Pc = if ($res) { $res } else { New-Counters } }
    elseif ($n -eq 'cores') { $script:Cores = [int]$res; if ($script:Info -and $script:Cores) { $script:Info.Cores = $script:Cores; if ($script:CpuSpec) { $script:CpuSpec.Tag.Text = "$($script:Info.Cpu), $($script:Cores) cores, $($script:Info.Threads) threads" } } }
    elseif ($n -eq 'dyn' -and $res -and $script:Info) {
      # v1.4: apply live hardware updates (Display/Monitor, Network, GPU, Storage, Health) without rebuilding the whole page
      if ($res.Display) {
        $script:Info.Display = $res.Display; $script:Info.Monitor = $res.Monitor
        if ($script:DispSpec) { $script:DispSpec.Tag.Text = ((@($res.Display, $res.Monitor) | Where-Object { $_ }) -join ', ') }
      }
      if ($res.Gpu) { $script:Info.Gpu = $res.Gpu; if ($script:GpuSpec) { $script:GpuSpec.Tag.Text = $res.Gpu } }
      if ($res.GpuDriver) {
        $script:Info.GpuDriver = $res.GpuDriver
        if ($script:GpuDrvSpec) { $script:GpuDrvSpec.Tag.Text = ((@("$($res.GpuDriver)", $(if ($script:Info.Vram) { "$(Size $script:Info.Vram) video memory" })) | Where-Object { $_ }) -join ', ') }
      }
      if ($res.Net) {
        $netChanged = ($res.Net -ne $script:Info.Net)
        $script:Info.Net = $res.Net
        if ($netChanged) { Start-Bg 'counters' $CounterScript }
      }
      if ($res.Disks) { $script:Info.Disks = $res.Disks; Fill-Disks $res.Disks }
      if ($res.Checks) { $script:Info.Checks = $res.Checks; $script:LastChecksTicks = [int64]$res.ChecksTicks; Fill-Health $script:Info }
    }
  }
  if ($script:Info -and $script:Pc -and -not $script:LiveTimer.IsEnabled) { Update-Live; $script:LiveTimer.Start(); $script:DynTimer.Start() }
  if (-not $script:Bg.Count) { $script:BgTimer.Stop() }
})
# Instant OS notifications for display/resolution changes and network connect/disconnect/IP changes
$script:OnDispChange = [EventHandler]{
  try { $w.Dispatcher.BeginInvoke([Action]{ Update-WindowScale; Start-DynRefresh }) | Out-Null } catch { } }
$script:OnNetAddr = [System.Net.NetworkInformation.NetworkAddressChangedEventHandler]{
  try { $w.Dispatcher.BeginInvoke([Action]{ Start-DynRefresh }) | Out-Null } catch { } }
$script:OnNetAvail = [System.Net.NetworkInformation.NetworkAvailabilityChangedEventHandler]{
  try { $w.Dispatcher.BeginInvoke([Action]{ Start-DynRefresh }) | Out-Null } catch { } }
try { [Microsoft.Win32.SystemEvents]::add_DisplaySettingsChanged($script:OnDispChange) } catch { }
try { [System.Net.NetworkInformation.NetworkChange]::add_NetworkAddressChanged($script:OnNetAddr) } catch { }
try { [System.Net.NetworkInformation.NetworkChange]::add_NetworkAvailabilityChanged($script:OnNetAvail) } catch { }
$w.Add_ContentRendered({
  $script:TWin = [int]((Get-Date) - $script:T0).TotalMilliseconds
  Start-Bg 'info' $InfoScript @($Data, $ChecksFile); Start-Bg 'counters' $CounterScript
  Start-Bg 'cores' '(Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum'
  $script:BgTimer.Start() })
$w.Add_Closed({
  $script:LiveTimer.Stop(); $script:DynTimer.Stop(); $script:BgTimer.Stop()
  try { [Microsoft.Win32.SystemEvents]::remove_DisplaySettingsChanged($script:OnDispChange) } catch { }
  try { [System.Net.NetworkInformation.NetworkChange]::remove_NetworkAddressChanged($script:OnNetAddr) } catch { }
  try { [System.Net.NetworkInformation.NetworkChange]::remove_NetworkAvailabilityChanged($script:OnNetAvail) } catch { } })
Set-PswDarkTitle $w
if ($SelfTest) {
  $script:TestTimer = New-Object System.Windows.Threading.DispatcherTimer; $script:TestTimer.Interval = [TimeSpan]::FromSeconds(8)
  $script:TestTimer.Add_Tick({ $script:TestTimer.Stop(); $c = $w.Content
    $bmp = [System.Windows.Media.Imaging.RenderTargetBitmap]::new([int]($c.ActualWidth * 2), [int]($c.ActualHeight * 2), 192, 192, [System.Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($c); $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder; $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bmp))
    $fs = [IO.File]::Create($SelfTest); $enc.Save($fs); $fs.Close()
    "window after $($script:TWin) ms, data after $($script:TData) ms, cores $($script:Info.Cores), counters $([bool]$script:Pc), disks $(@($script:Info.Disks).Count), net $($script:Info.Net)" | Set-Content "$SelfTest.txt"
    $w.Close() })
  $script:TestTimer.Start()
}
[void]$w.ShowDialog()
