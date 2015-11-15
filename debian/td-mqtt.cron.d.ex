#
# Regular cron jobs for the td-mqtt package
#
0 4	* * *	root	[ -x /usr/bin/td-mqtt_maintenance ] && /usr/bin/td-mqtt_maintenance
