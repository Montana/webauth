#!/usr/bin/perl

#Global Variables
$DEBUG = 0;
$LOGGING = 1;
$ENV{HTML_TEMPLATE_ROOT} = 'templates';
%PAGES = ('login'    => 'login2.tmpl',
          'cancel'   => 'cancel.tmpl',
          'verify'   => 'verify.tmpl',
          'error'    => 'error.tmpl');
$DEFAULT_CANCEL_PAGE="templates/cancel.html";
$TEST_COOKIE="WebloginTestCookie";
use strict;
use warnings;
use vars qw(%PAGES  $DEBUG $LOGGING $DEFAULT_CANCEL_PAGE $TEST_COOKIE);
use WebAuth3 qw(:base64 :const :krb5 :key);
use WebKDC;
use WebKDC::WebKDCException;
use CGI qw/:standard/;
use Dumpvalue;
use HTML::Template;
use CGI::Fast qw(-compile);
use CGI::Carp qw(carpout);
use CGI::Cookie;
use URI;


sub dump_stuff {
    my ($var, $val);
    foreach $var (sort(keys(%ENV))) {
	$val = $ENV{$var};
	$val =~ s|\n|\\n|g;
	$val =~ s|"|\\"|g;
	print "${var}=\"${val}\"\n";
    }
    
    print "\n";
    print "\n";
    while(<STDIN>) {
	print "INPUT: $_";
    }
}

sub print_headers {
    my ($q, $cookies) = @_;
    my $ca;

    my $secure = (defined($ENV{'HTTPS'}) && $ENV{'HTTPS'} eq 'on') ? 1 : 0;
    
    if ($cookies) {
	while (my($name, $value) = each(%{$cookies})) {
	    push(@$ca, $q->cookie(-name => $name, -value => $value, 
			  -secure => $secure));
	}
    }
    if  (!$q->cookie($TEST_COOKIE)) {
	push (@$ca, $q->cookie (-name    => $TEST_COOKIE,-value =>"True",
				-expires => '+1y',-path    => '/'));
    }

    if ($ca) {
	print $q->header(-type => 'text/html', -cookie => $ca);
    } else {
	print $q->header(-type => 'text/html');
    }
}


sub parse_uri {
    # Populate the new URI class object with the canonical return URL and make
    # sure that the scheme exists and is a valid WebAuth scheme.
    
    my ($lvars ,$resp) = @_;
    my $uri = URI->new ( $resp->return_url());

    $lvars->{return_url} = $uri->canonical;
    $lvars->{scheme} = $uri->scheme;
    if ((! defined $lvars->{scheme}) || ($lvars->{scheme} !~ /^https?/)) {
        $PAGES{error}->param ('err_bad_url' => 1);
        return 1;
    }
    $lvars->{host} = $uri->host;
    $lvars->{path} = $uri->path;
    $lvars->{port} = $uri->port if ($uri->port != 80 && $uri->port != 443);

    return 0;
}

sub print_login_page {
    my ($q, $lvars, $resp, $RT, $ST  ) = @_;

    $PAGES{login}->param ('username'   => $lvars->{username});
    $PAGES{login}->param ('RT' => $RT);
    $PAGES{login}->param ('ST' => $ST);
    $PAGES{login}->param ('LC' => $lvars->{LC});

    print_headers($q, $resp->proxy_cookies);
    #print $q->Dump;
    print $PAGES{'login'}->output;
}

sub print_error_cancel_page {
    my ($q, $page_id) = @_;

    print $q->header (-expires => 'now');
    print $PAGES{$page_id}->output;
}

sub fix_token { 
    my ($token) =@_;
    $token =~ tr/ /+/;
    return $token;
}

sub check_response_token {
   my ($q, $lvars) = @_;

    # Make sure there is always a response token set.
    if (! $lvars->{RT}) {
        $PAGES{error}->param ('err_response_token' => 1);
        print_error_cancel_page ($q, 'error');
        return 1;
    }
}

sub set_page_error {
    my ($q, $req) =@_;

    if ($q->param('Submit')&& $q->param('Submit') eq 'Login') {   
        # Set the error code and fill our username/password
	$PAGES{login}->param('err_password' => 1) unless $q->param('password');
	$PAGES{login}->param ('err_username' => 1) unless $q->param('username');
	$PAGES{login}->param ('err_cookies' => 1) unless $q->cookie($TEST_COOKIE);	
	$req->pass($q->param('password')) unless (!($q->param('password')));
	$req->user($q->param('username')) unless  (!($q->param('password')) || !($q->param('username')));
    } 

    return 0;
}


sub print_verify_page {
    my ($q, $lvars, $resp) = @_;

    my $pretty_return_url = $lvars->{scheme} ."://" . $lvars->{host};
    my $return_url = $resp->return_url();
    my $lc = $resp->login_canceled_token;

    $return_url .= "?WEBAUTHR=".$resp->response_token().";";
    $return_url .= ";WEBAUTHS=".$resp->app_state().";"	unless !$resp->app_state();

    # Set template parameters
    $PAGES{verify}->param ('return_url' => $return_url);
    $PAGES{verify}->param ('username' => ($resp->subject()));
    $PAGES{verify}->param ('pretty_return_url' => $pretty_return_url);

    #First time through always a "get" .. next time a redirect?
    $PAGES{verify}->param ('get' => 1);
    
    if (defined($lc)) {
	$PAGES{verify}->param ('login_cancel' =>1);
	my $cancel_url = $resp->return_url();
	$cancel_url .= "?WEBAUTHR=$lc;";
	$cancel_url .= ";WEBAUTHS=".$resp->app_state().";" 
	    unless !$resp->app_state();
	$PAGES{verify}->param ('cancel_url'=> $cancel_url);
   }

    # also need to check $resp->proxy_cookies() to see if we have
    # to update any proxy cookie
    print_headers($q, $resp->proxy_cookies);
    print $PAGES{verify}->output;

}

sub get_login_cancel_url {
    my ($lvars, $resp) = @_;
    my $lc = $resp->login_canceled_token;
    my $cancel_url;

    if ($lc) {
	$cancel_url = $resp->return_url() . "?WEBAUTHR=$lc;";
	$cancel_url .= ";WEBAUTHS=".$resp->app_state().";"  unless !$resp->app_state();
    }
    
    $cancel_url ? $PAGES{login}->param('cancel_url'=>$cancel_url):
	 $PAGES{login}->param('cancel_url'=>$DEFAULT_CANCEL_PAGE);

    $lvars->{LC} = (defined($cancel_url) && length($cancel_url)) ? 
	base64_encode($cancel_url) : "";    
    return 0;
}

#################################################################################
## Main
#################################################################################

%PAGES = map { $_ => HTML::Template->new (filename => $PAGES{$_},cache => 1) } (keys %PAGES) ;

while (my $q = new CGI::Fast) {
    my %varhash = map { $_ => $q->param ($_) } $q->param;
  
    my $req = new WebKDC::WebRequest;
    my $resp = new WebKDC::WebResponse;
    my $status;
    my $exception;
    
    # there always needs to be request token
    if (!defined($q->param('RT'))) {
	$PAGES{error}->param ('err_no_request_token' => 1);
	print_error_cancel_page ($q, 'error');
    
    } else {

        $status=set_page_error($q,$req);
	$req->service_token(fix_token($q->param('ST')));
	$req->request_token(fix_token($q->param('RT')));

        # pass in any proxy-tokens we have from a cookies
        # i.e., enumerate through all cookies that start with webauth_wpt
        # and put them into a hash:
	my %cart = fetch CGI::Cookie;
	foreach (keys %cart) {
	    if (/^webauth_wpt/) {
		my ($name, $val) = split('=', $cart{$_});
		$name=~ s/^(webauth_wpt_)//;
		$req->proxy_cookie($name,$q->cookie($_));
		print STDERR "found a cookie $q->cooki$name)\n"; 
	   }	
	}
 	
	($status, $exception) =
	    WebKDC::make_request_token_request($req, $resp);
	
	get_login_cancel_url(\%varhash,$resp); 
  
	if ($status ==WK_SUCCESS && $q->cookie($TEST_COOKIE) ) {
	    parse_uri(\%varhash, $resp);
	    print_verify_page($q, \%varhash, $resp );

	} elsif (($status == WK_ERR_USER_AND_PASS_REQUIRED)
		 || ($status == WK_ERR_LOGIN_FAILED)|| !($q->cookie($TEST_COOKIE)) ) {
	    # prompt for user/pass
	    print_login_page ($q, \%varhash, $resp, $req->request_token,$req->service_token);

       } else {  #errmsg
	    my $errmsg;
	    if ($status == WK_ERR_UNRECOVERABLE_ERROR) {
		# something nasty happened. display error message 
		$errmsg = "unrecoverable error occured. try again later.";

	    } elsif ($status == WK_ERR_REQUEST_TOKEN_STALE) {
		# user took too long to login, original request token is stale
		$errmsg = "you took too long to login";

	    } elsif ($status == WK_ERR_WEBAUTH_SERVER_ERROR) {
		# like WK_ERR_UNRECOVERABLE_ERROR, but indicates the error
		# most likely is due to the webauth server making the request,
		# so stop, but display a different error messaage.
		$errmsg = "there is most likely a configuration problem with" 
		    ." the server that redirected you. please contact its ".
		    "administrator";
	    }

            print STDERR "WebKDC::make_request_token_request failed with $errmsg: $exception\n";
	    $PAGES{error}->param ('err_webkdc' => 1);
	    $PAGES{error}->param ('err_msg'=> $errmsg);
	    print_error_cancel_page ($q, 'error');
	}
    }
    
} continue  { #matches fcgi
    # Clear out the template parameters for the next run.
    foreach (keys %PAGES) {
        $PAGES{$_}->clear_params ();
    }
    # Restart the script if the modification time changes.
    exit if -M $ENV{SCRIPT_FILENAME} < 0;
}

