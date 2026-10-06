from datetime import datetime

DESCRIPTION = 'Current local time on the Pi, as HH:MM; also shows it on the robot screen for 5 s.'


def run():
    now = datetime.now().strftime('%H:%M')
    return {'say': now, 'clock': now, 'seconds': 5}
