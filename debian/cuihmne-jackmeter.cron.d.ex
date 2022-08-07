#
# Regular cron jobs for the cuihmne-jackmeter package
#
0 4	* * *	root	[ -x /usr/bin/cuihmne-jackmeter_maintenance ] && /usr/bin/cuihmne-jackmeter_maintenance
