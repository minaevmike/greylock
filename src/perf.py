#!/usr/bin/python

import argparse
import json
import logging
import random
import requests
import sys
import time

logging.basicConfig(filename='perf.log',
        format='%(asctime)s %(levelname)s: perf: %(message)s',
        level=logging.INFO)
logging.getLogger().setLevel(logging.INFO)

class client:
    def __init__(self, seed, indexes_per_message, num_indexes, mailbox):
        random.seed(seed)

        self.max = 10000000
        self.sample = 'abcdefghijklmnopqrstuvwxyz0123456789.'

        self.indexes = []
        self.num_indexes = num_indexes
        self.gen_indexes(self.num_indexes)

        self.mailbox = mailbox

        self.max_indexes_per_message = indexes_per_message

    def rand_string(self, ln):
        return ''.join(random.sample(self.sample, ln))

    def msg_id(self):
        return self.rand_string(10) + '@' + self.rand_string(20)

    def gen_indexes(self, num):
        for i in range(num):
            self.indexes.append(self.rand_string(random.randint(5, 20)))

    def get_all_indexes(self):
        return self.indexes

    def get_indexes(self):
        return random.sample(self.indexes, self.max_indexes_per_message)

    def format_elliptics(self):

        indexes = {}
        indexes['text'] = ' '.join(self.get_indexes())

        ts = {}
        t = time.time()
        ts["tsec"] = int(t)
        ts["tnsec"] = int((t - int(t)) * 1000000000)

        doc = {}
        doc['id'] = self.msg_id()
        doc['bucket'] = ""
        doc['key'] = ""
        doc['timestamp'] = ts
        doc['index'] = indexes

        docs = {}
        docs['docs'] = [doc]
        docs['mailbox'] = self.mailbox

        return json.dumps(docs, encoding='utf8', ensure_ascii=False)

    def send(self, url, data):
        headers = {}
        timeout = 100

        #print data

        r = requests.post(url, data=data.decode('unicode_internal').encode('utf8'), headers=headers, timeout=timeout)
        if r.status_code != requests.codes.ok:
            raise RuntimeError("Could not update indexes: url: %s, status: %d" % (url, r.status_code))

    def generate_and_send(self, url):
        data = self.format_elliptics()
        self.send(url, data)

    def test(self, url):
        num = 0
        while True:
            start = time.time()
            self.generate_and_send(url)
            end = time.time()

            num += 1
            logging.info("%d %d/%d %f", num, self.max_indexes_per_message, self.num_indexes, (end - start) * 1000.0)
            print time.ctime(), num, self.max_indexes_per_message, self.num_indexes, (end - start) * 1000.0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description = 'Performance tester arguments')
    parser.add_argument('--url', dest='url', action='store', required=True,
            help='Remote URL to send index request')
    parser.add_argument('--mailbox', dest='mailbox', action='store', default='test@mailbox',
            help='Mailbox name where all inverted indexes will be stored')
    parser.add_argument('--num-indexes', dest='num_indexes', action='store', default=3000,
            help='Number of pregenerated indexes, every message to be indexed will contain subset of these indexes')
    parser.add_argument('--indexes-per-message', dest='indexes_per_message', action='store', default=1000,
            help='Number of random indexes (out of pregenerated max indexes) per message')
    parser.add_argument('--seed', dest='seed', action='store', default=time.time(),
            help='Random number generator seed')
    parser.add_argument('--store-indexes', dest='ifile', type=argparse.FileType('w'), default=sys.stdout,
            help='File to store all random indexes')

    args = parser.parse_args()
    if not args.mailbox:
        print("You must specify mailbox name to store all inverted indexes")
        exit(-1)

    c = client(seed = args.seed, indexes_per_message = args.indexes_per_message, num_indexes = args.num_indexes, mailbox = args.mailbox)
    args.ifile.write('\n'.join(c.get_all_indexes()))

    c.test(args.url)
