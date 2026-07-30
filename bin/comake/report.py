class Report(object):
    def __init__(self):
        self._indent = 0
        self._parent = None
        self._title = 'None'
        self._title_printed = False

    def title(self, title):
        self._title = title

    def sub(self):
        report = Report()
        report._indent = self._indent + 1
        report._parent = self
        return report

    def print_title(self):
        if self._parent and not self._parent._title_printed:
            self._parent.print_title()
        self.print_text("[%s]" % (self._title, ))
        self._title_printed = True

    def print_text(self, text):
        print((self._indent*'  ') + text)

