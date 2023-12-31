#!/usr/bin/perl
# Copyright (c) 2016-2023 The Hush developers
# Distributed under the GPLv3 software license, see the accompanying
# file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html

use strict;
use warnings;
use autodie;
use Data::Dumper;
my ($find,$replace,@files) = @ARGV;

usage() unless $find && defined $replace;
unless( @files ) {
    printf "No files to replace stuff!";
    exit 0;
}

my $fh;
print "Going to replace $find with $replace in " . scalar(@files) . " files\n";

for my $file (@files) {
    if (-d $file) {
        printf "Skipping directory $file\n";
        next;
    }
    unless ( -e $file ) {
        printf "$file does not exist!\n";
        next;
    }
    open $fh, '<', $file;
    my $content = join('',<$fh>);
    $content =~ s/\Q$find\E/$replace/g;
    close $fh;

    open $fh, '>', $file;
    print $fh $content;
    close $fh;
}

sub usage {
    die "$0 stringtofind stringtoreplace file [more files...]\n";
}
