#!/usr/bin/env python

import sys

from xml.sax import ContentHandler, make_parser

logs_list = []

class docHandler(ContentHandler):
    def __init__(self, *arg, **kw):
        ContentHandler.__init__(self, *arg, **kw)
        self.log_elements = {}
        self.log_current_tag = None

    def startElement(self, name, args):
        if name == u'log':
            self.log_elements = dict(args)
            return
        self.log_current_tag = name

    def characters(self, context):
        if self.log_current_tag:
            self.log_elements.setdefault(self.log_current_tag, []).append(context)

    def endElement(self, name):
        if name == u'log':
            log = {}
            for key, value in self.log_elements.items():
                log[key] = ''.join(value).strip()
            if log:
                log['driver_index'] = int(log['driver_index'])
                logs_list.append(log)
            self.log_current_tag = None

dh = docHandler()
parser = make_parser()
parser.setContentHandler(dh)
print("Loading...")
parser.parse(open(sys.argv[1]))
print("Sorting %d logs..." % (len(logs_list), ))
logs_list.sort(key=lambda x: x['driver_index'])

def print_logs(logs_list):
    last_index = [None]
    missing_logs = [0]
    def print_log(data):
        driver_index = data['driver_index']
        if last_index[0]:
            if last_index[0] + 1 != driver_index:
                missing_logs[0] += 1
            last_index[0] = driver_index
        f = "%s:" % (data['function'], )
        f = ''
        print(str(driver_index) + '|' + data['timestamp'] + '|' +
              data['module'] + '|' + data['file'] + ':' + data['function'] + ':' +
              data['line'] + '|' + data['string'])

    for data in logs_list:
        print_log(data)

    if missing_logs[0]:
        print("Missing logs: %d\n" % (missing_logs[0], ))

print_logs(logs_list)
