#!/usr/bin/env python2
# Copyright (c) 2016-2023 The Hush developers
# Copyright (c) 2018 The Zcash developers
# Distributed under the GPLv3 software license, see the accompanying
# file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html

#
# Test proper expiry for transactions >= version 3
#

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, \
    connect_nodes_bi, sync_blocks, start_nodes, \
    wait_and_assert_operationid_status

from decimal import Decimal

class MempoolTxExpiryTest(BitcoinTestFramework):

    def setup_nodes(self):
        return start_nodes(4, self.options.tmpdir, [["-nuparams=5ba81b19:205", "-txexpirydelta=4", "-debug=mempool"]] * 4)

    # Test before, at, and after expiry block
    # chain is at block height 199 when run_test executes
    def run_test(self):
        alice = self.nodes[0].getnewaddress()
        z_alice = self.nodes[0].z_getnewaddress()
        bob = self.nodes[2].getnewaddress()
        z_bob = self.nodes[2].z_getnewaddress()

        # When Overwinter not yet activated, no expiryheight in tx
        sapling_tx = self.nodes[0].sendtoaddress(bob, 0.01)
        rawtx = self.nodes[0].getrawtransaction(sapling_tx, 1)
        assert_equal(rawtx["overwintered"], False)
        assert("expiryheight" not in rawtx)

        self.nodes[0].generate(6)
        self.sync_all()

        print "Splitting network..."
        self.split_network()

        # When Overwinter is activated, test dependent txs
        firstTx = self.nodes[0].sendtoaddress(alice, 0.1)
        firstTxInfo = self.nodes[0].getrawtransaction(firstTx, 1)
        print "First tx expiry height:", firstTxInfo['expiryheight']
        # Mine first transaction
        self.nodes[0].generate(1)
        for outpoint in firstTxInfo['vout']:
            if outpoint['value'] == Decimal('0.10000000'):
                vout = outpoint
                break
        inputs = [{'txid': firstTx, 'vout': vout['n'], 'scriptPubKey': vout['scriptPubKey']['hex']}]
        outputs = {alice: 0.1}
        rawTx = self.nodes[0].createrawtransaction(inputs, outputs)
        rawTxSigned = self.nodes[0].signrawtransaction(rawTx)
        assert(rawTxSigned['complete'])
        secondTx = self.nodes[0].sendrawtransaction(rawTxSigned['hex'])
        secondTxInfo = self.nodes[0].getrawtransaction(secondTx, 1)
        print "Second tx expiry height:", secondTxInfo['expiryheight']
        # Mine second, dependent transaction
        self.nodes[0].generate(1)
        print "Mine 6 competing blocks on Node 2..."
        blocks = self.nodes[2].generate(6)
        print "Connect nodes to force a reorg"
        connect_nodes_bi(self.nodes,0,2)
        self.is_network_split = False
        print "Syncing blocks"
        sync_blocks(self.nodes)
        print "Ensure that both txs are dropped from mempool of node 0"
        print "Blockheight node 0:", self.nodes[0].getblockchaininfo()['blocks']
        print "Blockheight node 2:", self.nodes[2].getblockchaininfo()['blocks']
        assert_equal(set(self.nodes[0].getrawmempool()), set())
        assert_equal(set(self.nodes[2].getrawmempool()), set())

        ## Shield one of Alice's coinbase funds to her zaddr
        res = self.nodes[0].z_shieldcoinbase("*", z_alice, 0.0001, 1)
        wait_and_assert_operationid_status(self.nodes[0], res['opid'])
        self.nodes[0].generate(1)
        self.sync_all()

        # Get balance on node 0
        bal = self.nodes[0].z_gettotalbalance()
        print "Balance before zsend, after shielding 10: ", bal
        assert_equal(Decimal(bal["private"]), Decimal("9.9999"))

        print "Splitting network..."
        self.split_network()

        # Create transactions
        blockheight = self.nodes[0].getblockchaininfo()['blocks']
        zsendamount = Decimal('1.0') - Decimal('0.0001')
        recipients = []
        recipients.append({"address": z_bob, "amount": zsendamount})
        myopid = self.nodes[0].z_sendmany(z_alice, recipients)
        persist_shielded = wait_and_assert_operationid_status(self.nodes[0], myopid)
        persist_transparent = self.nodes[0].sendtoaddress(bob, 0.01)
        # Verify transparent transaction is version 3 intended for Overwinter branch
        rawtx = self.nodes[0].getrawtransaction(persist_transparent, 1)
        assert_equal(rawtx["version"], 3)
        assert_equal(rawtx["overwintered"], True)
        assert_equal(rawtx["expiryheight"], blockheight + 5)
        print "Blockheight at persist_transparent & persist_shielded creation:", self.nodes[0].getblockchaininfo()['blocks']
        print "Expiryheight of persist_transparent:", rawtx['expiryheight']
        # Verify shielded transaction is version 3 intended for Overwinter branch
        rawtx = self.nodes[0].getrawtransaction(persist_shielded, 1)
        print "Expiryheight of persist_shielded", rawtx['expiryheight']
        assert_equal(rawtx["version"], 3)
        assert_equal(rawtx["overwintered"], True)
        assert_equal(rawtx["expiryheight"], blockheight + 5)

        print "\n Blockheight advances to less than expiry block height. After reorg, txs should persist in mempool"
        assert(persist_transparent in self.nodes[0].getrawmempool())
        assert(persist_shielded in self.nodes[0].getrawmempool())
        assert_equal(set(self.nodes[2].getrawmempool()), set())
        print "mempool node 0:", self.nodes[0].getrawmempool()
        print "mempool node 2:", self.nodes[2].getrawmempool()
        bal = self.nodes[0].z_gettotalbalance()
        print "Printing balance before persist_shielded & persist_transparent are initially mined from mempool", bal
        # Txs are mined on node 0; will later be rolled back
        self.nodes[0].generate(1)
        print "Node 0 generated 1 block"
        print "Node 0 height:", self.nodes[0].getblockchaininfo()['blocks']
        print "Node 2 height:", self.nodes[2].getblockchaininfo()['blocks']
        bal = self.nodes[0].z_gettotalbalance()
        print "Printing balance after persist_shielded & persist_transparent are mined:", bal
        assert_equal(set(self.nodes[0].getrawmempool()), set())

        print "Mine 2 competing blocks on Node 2..."
        blocks = self.nodes[2].generate(2)
        for block in blocks:
            blk = self.nodes[2].getblock(block)
            print "Height: {0}, Mined block txs: {1}".format(blk["height"], blk["tx"])
        print "Connect nodes to force a reorg"
        connect_nodes_bi(self.nodes,0,2)
        self.is_network_split = False

        print "Syncing blocks"
        sync_blocks(self.nodes)

        print "Ensure that txs are back in mempool of node 0"
        print "Blockheight node 0:", self.nodes[0].getblockchaininfo()['blocks']
        print "Blockheight node 2:", self.nodes[2].getblockchaininfo()['blocks']
        print "mempool node 0: ", self.nodes[0].getrawmempool()
        print "mempool node 2: ", self.nodes[2].getrawmempool()
        assert(persist_transparent in self.nodes[0].getrawmempool())
        assert(persist_shielded in self.nodes[0].getrawmempool())
        bal = self.nodes[0].z_gettotalbalance()
        # Mine txs to get them out of the way of mempool sync in split_network()
        print "Generating another block on node 0 to clear txs from mempool"
        self.nodes[0].generate(1)
        assert_equal(set(self.nodes[0].getrawmempool()), set())
        sync_blocks(self.nodes)

        print "Splitting network..."
        self.split_network()

        print "\n Blockheight advances to equal expiry block height. After reorg, txs should persist in mempool"
        myopid = self.nodes[0].z_sendmany(z_alice, recipients)
        persist_shielded_2 = wait_and_assert_operationid_status(self.nodes[0], myopid)
        persist_transparent_2 = self.nodes[0].sendtoaddress(bob, 0.01)
        rawtx_trans = self.nodes[0].getrawtransaction(persist_transparent_2, 1)
        rawtx_shield = self.nodes[0].getrawtransaction(persist_shielded_2, 1)
        print "Blockheight node 0 at persist_transparent_2 creation:", self.nodes[0].getblockchaininfo()['blocks']
        print "Blockheight node 2 at persist_transparent_2 creation:", self.nodes[2].getblockchaininfo()['blocks']
        print "Expiryheight of persist_transparent_2:", rawtx_trans['expiryheight']
        print "Expiryheight of persist_shielded_2:", rawtx_shield['expiryheight']
        blocks = self.nodes[2].generate(4)
        for block in blocks:
            blk = self.nodes[2].getblock(block)
            print "Height: {0}, Mined block txs: {1}".format(blk["height"], blk["tx"])
        print "Connect nodes to force a reorg"
        connect_nodes_bi(self.nodes, 0, 2)
        self.is_network_split = False
        sync_blocks(self.nodes)
        print "Ensure that persist_transparent_2 & persist_shielded_2 are in mempool at expiry block height"
        print "Blockheight node 0:", self.nodes[0].getblockchaininfo()['blocks']
        print "Blockheight node 2:", self.nodes[2].getblockchaininfo()['blocks']
        print "mempool node 0: ", self.nodes[0].getrawmempool()
        print "mempool node 2: ", self.nodes[2].getrawmempool()
        assert(persist_transparent_2 in self.nodes[0].getrawmempool())
        assert(persist_shielded_2 in self.nodes[0].getrawmempool())
        # Mine persist txs to get them out of the way of mempool sync in split_network()
        self.nodes[0].generate(1)
        assert_equal(set(self.nodes[0].getrawmempool()), set())
        sync_blocks(self.nodes)
        print "Balance after persist_shielded_2 is mined to remove from mempool: ", self.nodes[0].z_gettotalbalance()

        print "Splitting network..."
        self.split_network()

        print "\n Blockheight advances to greater than expiry block height. After reorg, txs should expire from mempool"
        print "Balance before expire_shielded is sent: ", self.nodes[0].z_gettotalbalance()
        myopid = self.nodes[0].z_sendmany(z_alice, recipients)
        expire_shielded = wait_and_assert_operationid_status(self.nodes[0], myopid)
        expire_transparent = self.nodes[0].sendtoaddress(bob, 0.01)
        print "Blockheight node 0 at expire_transparent creation:", self.nodes[0].getblockchaininfo()['blocks']
        print "Blockheight node 2 at expire_shielded creation:", self.nodes[2].getblockchaininfo()['blocks']
        print "Expiryheight of expire_transparent:", self.nodes[0].getrawtransaction(expire_transparent, 1)['expiryheight']
        print "Expiryheight of expire_shielded:", self.nodes[0].getrawtransaction(expire_shielded, 1)['expiryheight']
        assert(expire_transparent in self.nodes[0].getrawmempool())
        assert(expire_shielded in self.nodes[0].getrawmempool())
        blocks = self.nodes[2].generate(6)
        for block in blocks:
            blk = self.nodes[2].getblock(block)
            print "Height: {0}, Mined block txs: {1}".format(blk["height"], blk["tx"])
        print "Connect nodes to force a reorg"
        connect_nodes_bi(self.nodes, 0, 2)
        self.is_network_split = False
        sync_blocks(self.nodes)
        print "Ensure that expire_transparent & expire_shielded are in mempool at expiry block height"
        print "mempool node 0: ", self.nodes[0].getrawmempool()
        print "mempool node 2: ", self.nodes[2].getrawmempool()
        assert_equal(set(self.nodes[0].getrawmempool()), set())
        print "Ensure balance of node 0 is correct"
        bal = self.nodes[0].z_gettotalbalance()
        print "Balance after expire_shielded has expired: ", bal
        assert_equal(Decimal(bal["private"]), Decimal("7.9999"))


if __name__ == '__main__':
    MempoolTxExpiryTest().main()
