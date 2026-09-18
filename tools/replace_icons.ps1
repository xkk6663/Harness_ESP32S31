$f = 'C:\Users\xiao1\Desktop\esp32\display_audio_photo\main\ui\overlay_wallpaper.c'
$s = [IO.File]::ReadAllText($f)
$map = @{
    '&image_wifi' = 'ICON_WIFI'
    '&image_sun' = 'ICON_SUN'
    '&image_cloud' = 'ICON_CLOUD'
    '&image_temp' = 'ICON_TEMP'
    '&image_humidity' = 'ICON_HUMIDITY'
    '&image_wind' = 'ICON_WIND'
    '&image_sunrise' = 'ICON_SUNRISE'
    '&image_rain' = 'ICON_RAIN'
}
foreach ($k in $map.Keys) {
    $s = $s.Replace($k, $map[$k])
}
[IO.File]::WriteAllText($f, $s, (New-Object Text.UTF8Encoding $false))
Write-Host "done"
