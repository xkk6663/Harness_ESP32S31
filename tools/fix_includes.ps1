$map = @{
  'middleware/audio_service.h'   = 'middleware/audio/audio_service.h'
  'middleware/udp_voice.h'       = 'middleware/audio/udp_voice.h'
  'middleware/record_service.h'  = 'middleware/audio/record_service.h'
  'middleware/wifi_manager.h'   = 'middleware/net/wifi_manager.h'
  'middleware/link_manager.h'    = 'middleware/net/link_manager.h'
  'middleware/session_manager.h'= 'middleware/net/session_manager.h'
  'middleware/net_time_service.h'='middleware/net/net_time_service.h'
  'middleware/fs_service.h'     = 'middleware/periph/fs_service.h'
  'middleware/led_service.h'    = 'middleware/periph/led_service.h'
  'middleware/button_service.h' = 'middleware/periph/button_service.h'
  'middleware/camera_service.h'  = 'middleware/camera/camera_service.h'
  'middleware/color_hsv.h'       = 'middleware/camera/color_hsv.h'
}
$root = 'C:\Users\xiao1\Desktop\esp32\display_audio_photo\main'
$files = Get-ChildItem $root -Recurse -Include '*.c','*.h'
foreach ($f in $files) {
  $s = [IO.File]::ReadAllText($f.FullName)
  $orig = $s
  foreach ($k in $map.Keys) {
    $s = $s.Replace("`"$k`"", "`"$($map[$k])`"")
  }
  if ($s -ne $orig) {
    [IO.File]::WriteAllText($f.FullName, $s, (New-Object Text.UTF8Encoding $false))
    Write-Host "updated: $($f.FullName)"
  }
}
