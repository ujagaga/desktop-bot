import json
from datetime import datetime
from urllib.parse import urlencode
from urllib.request import urlopen

DESCRIPTION = ("Today's weather forecast for Novi Sad: current temperature and conditions, "
               "and when rain or snow will start or stop today.")

LATITUDE, LONGITUDE = 45.243, 19.819
# WMO weather codes used by Open-Meteo; 51 and above are precipitation.
CONDITIONS = {0: 'clear sky', 1: 'mainly clear', 2: 'partly cloudy', 3: 'overcast', 45: 'fog', 48: 'fog',
              51: 'light drizzle', 53: 'drizzle', 55: 'heavy drizzle', 56: 'freezing drizzle',
              57: 'freezing drizzle', 61: 'light rain', 63: 'rain', 65: 'heavy rain', 66: 'freezing rain',
              67: 'freezing rain', 71: 'light snow', 73: 'snow', 75: 'heavy snow', 77: 'snow grains',
              80: 'rain showers', 81: 'rain showers', 82: 'heavy rain showers', 85: 'snow showers',
              86: 'heavy snow showers', 95: 'thunderstorm', 96: 'thunderstorm with hail',
              99: 'thunderstorm with hail'}


def run():
    query = urlencode({'latitude': LATITUDE, 'longitude': LONGITUDE, 'current': 'temperature_2m,weather_code',
                       'hourly': 'weather_code', 'timezone': 'auto', 'forecast_days': 1})
    with urlopen('https://api.open-meteo.com/v1/forecast?' + query, timeout=8) as response:
        data = json.load(response)
    code = data['current']['weather_code']
    text = f"{round(data['current']['temperature_2m'])} °C, {CONDITIONS.get(code, 'unknown conditions')}."
    raining = code >= 51
    now = datetime.now()
    for time, hourly_code in zip(data['hourly']['time'], data['hourly']['weather_code']):
        if datetime.fromisoformat(time) > now and (hourly_code >= 51) != raining:
            change = 'stop' if raining else f"start ({CONDITIONS.get(hourly_code, 'precipitation')})"
            return text + f" Precipitation should {change} around {time[-5:]}."
    return text + (' Precipitation should continue for the rest of the day.' if raining
                   else ' No precipitation expected for the rest of the day.')
