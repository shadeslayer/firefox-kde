#!/usr/bin/perl

use strict;
use warnings;
use XML::Simple;
use JSON;
use File::Basename;

sub check_matches {
    my $plugin = shift;
    my $conditions = shift;
    (ref($conditions) eq "HASH") or die "Ah, shit";

    my $matches = 0;
    if (ref($plugin) eq "ARRAY") {
        foreach my $subvalue (@{$plugin}) {
            $matches ||= check_matches($subvalue, $conditions);
        }
        return $matches;
    }

    (ref($plugin) eq "HASH") or return 0;
    (scalar(keys(%$conditions)) == 0) && return 1;

    foreach my $key (keys(%$conditions)) {
        $matches = 0;
        if (ref($conditions->{$key}) eq "HASH") {
            $matches = check_matches($plugin->{$key}, $conditions->{$key});
        } elsif (ref($conditions->{$key}) eq "ARRAY") {
            $matches = 1;
            foreach my $condition (@{$conditions->{$key}}) {
                $matches &&= check_matches($plugin->{$key}, $condition);
            }
        } else {
            if (exists($plugin->{$key}) and ref($plugin->{$key}) ne "HASH" and ref($plugin->{$key}) ne "ARRAY") {
                $matches = $conditions->{$key} =~ /[\+\?\.\*\^\$\(\)\[\]\{\}\|\\]*/ ? 
                            ($plugin->{$key} =~ /$conditions->{$key}/) :
                            ($plugin->{$key} eq $conditions->{$key});
            }
        }
        last if not $matches;
    }

    return $matches;
}

sub verify_overrides {
    my $dir = shift;
    my $lang = shift;
    my $config = shift;
    my $overrides = shift;

    my @plugins = sort(<$dir/$lang/*.xml>);
    foreach my $plugin (@plugins) {
        my $xml = new XML::Simple;
        my $data = $xml->XMLin($plugin, keyattr => []);

        my $matches = 0;
        foreach my $conditions (@{$config->{'matches'}}) {
            $matches = check_matches($data, $conditions);
            last if $matches;
        }
        foreach my $conditions (@{$config->{'ignore'}}) {
            $matches &&= not check_matches($data, $conditions);
            last if not $matches;
        }

        my $id = basename($plugin);
        $id =~ s/\.xml$//;
        my @arr = @{$overrides->{$lang}};
        my ($index) = grep($arr[$_] eq $id, 0..$#arr);
        $matches ^ defined($index) and die "Verification failed for $id search plugin in $lang";
    }
}

my $dir;
my %overrides;
my $config;

while (@ARGV) {
    my $arg = shift;
    if ($arg eq '-d') {
        $dir = shift;
    }
}

defined($dir) or die "Missing options";

open(CONFIG, "debian/searchplugins/compute-overrides.json") or die "Cannot open config";
my $json;
while (<CONFIG>) { $json .= $_; }
close(CONFIG);
$config = JSON::decode_json($json);

open(LIST, "debian/config/search-mods.list") and do {
    my $parsing = 0;
    while(<LIST>) {
        if (/^\[Overrides\]$/) {
            $parsing = 1;
        } elsif ($parsing == 1) {
            if (/^\[/) {
                $parsing = 0;
            } elsif (/^([^\:]*)\:(.*)/) {
                my @l = split(',', $2);
                map($_ = basename($_), @l);
                map($_ =~ s/\.xml$//, @l);
                $overrides{$1} = \@l;
            }
        }
    }
    close(LIST);

    open(SHIPPED_LOCALES, "debian/config/locales.shipped") or die "Cannot open list of shipped locales";
    while(<SHIPPED_LOCALES>) {
        $_ =~ s/#.*//; s/\s*$//;
        /^([^\:]*)\:.*/ and do {
            verify_overrides($dir, $1, $config, \%overrides);
        }
    }
    close(SHIPPED_LOCALES);
}
