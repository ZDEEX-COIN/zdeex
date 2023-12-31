#!/usr/bin/env python2
# Copyright (c) 2016-2023 The Hush developers
# Copyright (c) 2014 The Bitcoin Core developers
# Distributed under the GPLv3 software license, see the accompanying
# file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html

#
# Exercise API with -disablewallet.
#

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import initialize_chain_clean, start_nodes


class DisableWalletTest (BitcoinTestFramework):

    def setup_chain(self):
        print("Initializing test directory "+self.options.tmpdir)
        initialize_chain_clean(self.options.tmpdir, 1)

    def setup_network(self, split=False):
        self.nodes = start_nodes(1, self.options.tmpdir, [['-disablewallet']])
        self.is_network_split = False
        self.sync_all()

    def run_test (self):
        # Check regression: https://github.com/bitcoin/bitcoin/issues/6963#issuecomment-154548880
        x = self.nodes[0].validateaddress('t3b1jtLvxCstdo1pJs9Tjzc5dmWyvGQSZj8')
        assert(x['isvalid'] == False)
        x = self.nodes[0].validateaddress('tmGqwWtL7RsbxikDSN26gsbicxVr2xJNe86')
        assert(x['isvalid'] == True)

if __name__ == '__main__':
    DisableWalletTest ().main ()
