#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;

my $app;
my $spdir;
my $basedir;

my %wanted_overrides = (
    "en-US" => []
);
my %wanted_additions = (
    "en-US" => []
);

my %overrides;
my %additions;

while (@ARGV) {
    my $arg = shift;
    if ($arg eq '-a') {
        $app = shift;
    } elsif ($arg eq '-d') {
        $spdir = shift;
    } elsif ($arg eq '-b') {
        $basedir = shift;
    }
}

open(ORIG, "debian/config/search-mods.list") and do {
    my %map = (
        "Overrides" => \%overrides,
        "Additions" => \%additions
    );
    my $table;
    while(<ORIG>) {
        chomp;
        $_ =~ s/#.*//; s/\s*$//;
        if (/^\[([[:alnum:]]*)\]$/) {
            $table = $map{$1};
        } elsif (/^([^\:]*)\:(.*)/) {
            my @l = sort({basename($a) cmp basename($b)} split(',', $2));
            $table->{$1} = \@l;
        } elsif (not /^$/) { die "Unexpected line"; }
    }
};
close(ORIG);

sub parse_manifest {
    my $lang = shift;
    my $path = shift;

    my %map = (
        "Overrides" => \%wanted_overrides,
        "Additions" => \%wanted_additions
    );
    my $table;
    open(FILE, $path) or die "Failed to open manifest $path";
    while(<FILE>) {
        chomp;
        $_ =~ s/#.*//; s/\s*$//;
        if (/^\[([[:alnum:]]*)\]$/) {
            $table = $map{$1};
        } elsif (/^(.+)$/) {
            if (not exists($table->{$lang})) {
                $table->{$lang} = [];
            }
            push(@{$table->{$lang}}, $1);
        } elsif (not /^$/) { die "Unexpected line"; }
    }
    close(FILE);
}

opendir(DIR, "debian/searchplugins") and do {
    my @files = readdir(DIR);
    foreach my $file (@files) {
        if (-d "debian/searchplugins/$file" and 
            -e "debian/searchplugins/$file/list.txt") {
            parse_manifest($file, "debian/searchplugins/$file/list.txt");
        } elsif (-f "debian/searchplugins/$file" and $file eq "list.txt") {
            parse_manifest("en-US", "debian/searchplugins/$file")
        }
    }
};
closedir(DIR);

sub handle_locale {
    my $lang = shift;
    my $path = shift;

    $overrides{$lang} = [];
    $additions{$lang} = [];

    my $ov = $wanted_overrides{exists($wanted_overrides{$lang}) ? $lang : "en-US"};
    my $ad = $wanted_additions{exists($wanted_additions{$lang}) ? $lang : "en-US"};

    my @upstream;
    open(FILE, $path) and do {
        while(<FILE>) { s/\r\n/\n/; chomp; push(@upstream, $_); }
    };
    close(FILE);

    foreach my $override (@{$ov}) {
        my @files = <debian/searchplugins/$lang/$override.xml>;
        @files = grep(-f $_, @files);
        scalar(@files) > 0 or @files = <debian/searchplugins/en-US/$override.xml>;
        @files = grep(-f $_, @files);
        scalar(@files) > 0 or die "No source plugin for override $override in $lang";

        foreach my $file (@files) {
            my $base = basename($file);
            $base =~ s/\.xml$//;
            $file =~ s/debian\/searchplugins\///;
            my ($index) = grep($upstream[$_] eq $base, 0..$#upstream);
            defined($index) and push(@{$overrides{$lang}}, $file);
        }
    }

    foreach my $addition (@{$ad}) {
        my @files = <debian/searchplugins/$lang/$addition.xml>;
        @files = grep(-f $_, @files);
        scalar(@files) > 0 or @files = <debian/searchplugins/en-US/$addition.xml>;
        @files = grep(-f $_, @files);
        scalar(@files) > 0 or die "No source plugin for additional plugin $addition in $lang";

        foreach my $file (@files) {
            my $base = basename($file);
            $base =~ s/\.xml$//;
            $file =~ s/debian\/searchplugins\///;
            my ($index) = grep($upstream[$_] eq $base, 0..$#upstream);
            defined($index) and die "Cannot add plugin $base for $lang. Destination already exists";
            push(@{$additions{$lang}}, $file);
        }
    }

    @{$overrides{$lang}} = sort({basename($a) cmp basename($b)} @{$overrides{$lang}});
    @{$additions{$lang}} = sort({basename($a) cmp basename($b)} @{$additions{$lang}});

    if (scalar(@{$overrides{$lang}}) == 0) { delete $overrides{$lang}; }
    if (scalar(@{$additions{$lang}}) == 0) { delete $additions{$lang}; }
}

open(SHIPPED_LOCALES, "debian/config/locales.shipped") or die "Cannot open shipped locales";
while(<SHIPPED_LOCALES>) {
    $_ =~ s/#.*//; s/\s*$//;
    /^([^\:]*)\:.*/ and do {
        handle_locale($1, "$basedir/l10n/$1/$app/$spdir/list.txt");
    }
}
close(SHIPPED_LOCALES);

handle_locale("en-US", "$basedir/$app/locales/en-US/$spdir/list.txt");

scalar(keys(%overrides)) > 0 or scalar(keys(%additions)) > 0 or do {
    unlink("debian/config/search-mods.list");
    exit(0);
};

open(OUTFILE, ">debian/config/search-mods.list");
scalar(keys(%overrides)) > 0 and do {
    print OUTFILE "[Overrides]\n";
    foreach my $key (sort(keys(%overrides))) {
        my $val = join(",", @{$overrides{$key}});
        print OUTFILE "$key:$val\n";
    }
};

scalar(keys(%additions)) > 0 and do {
    print OUTFILE "\n[Additions]\n";
    foreach my $key (sort(keys(%additions))) {
        my $val = join(",", @{$additions{$key}});
        print OUTFILE "$key:$val\n";
    }
};
close(OUTFILE);
